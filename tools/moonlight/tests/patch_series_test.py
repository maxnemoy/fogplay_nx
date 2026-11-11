#!/usr/bin/env python3
"""Real pinned Git fixtures: replay, fail-closed state, rollback and cache integrity."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/moonlight'))
import prepare
import headers
import manifest
import check
import fetch

spec = importlib.util.spec_from_file_location('moonlight_checker', ROOT / 'tools/moonlight/check-source-patches.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def snapshot(root):
    """Include file bytes/type/mode/mtime and links, never follow a fixture link."""
    result = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        directories[:] = [name for name in directories if name not in ('.git', '__pycache__')]
        for name in directories + files:
            path = Path(directory) / name
            if name in ('.git', '.prepare.lock', '.include.lock'):
                continue
            info = path.lstat()
            value = os.readlink(path) if path.is_symlink() else (
                prepare.digest(path) if stat.S_ISREG(info.st_mode) else None)
            result[str(path.relative_to(root))] = (
                info.st_mode, info.st_mtime_ns if not stat.S_ISDIR(info.st_mode) else None, value)
    return result


class SeriesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='moonlight-series-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.series = prepare.load_series(ROOT)
        shutil.copytree(ROOT / 'tools/moonlight', self.root / 'tools/moonlight',
                        ignore=shutil.ignore_patterns('__pycache__'))
        for repo in self.series['repositories']:
            checker.archive(ROOT / repo['path'], self.root / repo['path'], repo['revision'])
        (self.root / prepare.BASE / '.prepare.lock').touch()
        self.prepared = self.root / prepare.PREPARED
        self.sentinel = self.root / prepare.BASE / 'unrelated.keep'
        self.sentinel.write_bytes(b'preserve me')

    def write_series(self, series=None):
        (self.root / prepare.SERIES).write_text(json.dumps(series or self.series, indent=2) + '\n')

    def rejected(self, operation=None, pattern=None):
        before = snapshot(self.root)
        with self.assertRaises((prepare.PrepareError, OSError, subprocess.CalledProcessError)) as caught:
            (operation or (lambda: prepare.apply(self.root)))()
        if pattern:
            self.assertIn(pattern, str(caught.exception))
        self.assertEqual(before, snapshot(self.root), str(caught.exception))

    def base_tree(self):
        repos = prepare.repository_map(self.series)
        for output in self.series['outputs']:
            repo = repos[output['repository']]
            target = self.prepared / output['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(prepare.git_file(self.root / repo['path'], repo['revision'], output['source_path']))

    def frozen_tree(self):
        self.base_tree()
        for patch in self.series['patches'][:8]:
            prepare.apply_patch(self.prepared, self.root / prepare.SERIES.parent / patch['path'])
        actual, _ = prepare.scan_tree(self.prepared)
        self.assertEqual(actual, {o['path']: o['frozen_sha256'] for o in self.series['outputs']})

    def test_first_eight_patches_retain_frozen_identity(self):
        expected = (
            'b33133f37e1a58df1467b19d90691fd2d674130285c4b64066a056f1b9962cde',
            '0da807ef992fb0ebc79d238037a02f2d07eec236920f34196e9aca78205a09f4',
            '0fafb47d527381bde642ca7a2290c5d71778182a17c5d37612b03df6e7d94e81',
            'f19bb4ed0e52d8dc72c032b4d390ec4683f0034429c8cbeec6e4e5065cd04282',
            'dfb5a2022f5874f2c74587f8f6d2273ab2d626ede77930b3c4d9610e43171791',
            '3eb9444685fb3ed23c44a7bbc810a4a426c7a35e88d3bdbee9c4da994e8a521c',
            'd2b9b0290c897536c6cb96afb7c5670409f7789ed346e5ae9775095790366b86',
            '63a4b3fbae63b51886362430c0455d8ab442da7ecbb183c7fa0defb323ac87ea',
        )
        self.assertEqual(tuple(p['sha256'] for p in self.series['patches'][:8]), expected)
        self.frozen_tree()
        aggregate = hashlib.sha256()
        for output in sorted(self.series['outputs'], key=lambda output: output['path']):
            aggregate.update(output['path'].encode() + b'\0' + output['frozen_sha256'].encode() + b'\n')
        self.assertEqual(aggregate.hexdigest(), '47551be611ce395297c367f01e70e60839533b6bdb3b7821bc0be3ad63db784a')

    def previous_tree(self):
        self.frozen_tree()
        prepare.apply_patch(self.prepared, self.root / prepare.SERIES.parent / self.series['patches'][8]['path'])
        actual, _ = prepare.scan_tree(self.prepared)
        self.assertEqual(actual, {o['path']: o['previous_sha256'] for o in self.series['outputs']})

    def test_final_tree_restores_exact_frozen_bytes(self):
        self.frozen_tree()
        self.assertEqual(prepare.classify_prepared(self.root, self.series), 'prepared')
        before = snapshot(self.root)
        self.assertEqual(prepare.apply(self.root), 'unchanged')
        self.assertEqual(prepare.apply(self.root, check=True), 'checked')
        self.assertEqual(before, snapshot(self.root))

    def test_exact_previous_migration_and_rollback(self):
        self.previous_tree()
        self.assertEqual(prepare.classify_prepared(self.root, self.series), 'previous')
        self.rejected(lambda: prepare.apply(self.root, check=True), pattern='exact final')
        original = Path.rename
        calls = 0
        def rename(path, target):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('injected previous migration publication failure')
            return original(path, target)
        with mock.patch.object(Path, 'rename', rename):
            self.rejected(pattern='injected previous migration')
        self.assertEqual(prepare.classify_prepared(self.root, self.series), 'previous')
        self.assertEqual(prepare.apply(self.root), 'prepared')
        self.assertEqual(prepare.apply(self.root, check=True), 'checked')

    def test_frozen_checkpoint_cannot_be_skipped(self):
        self.series['outputs'][0]['frozen_sha256'] = '0' * 64
        self.write_series()
        self.rejected(pattern='first eight patches differ from frozen C7 hash')

    def test_previous_checkpoint_cannot_be_skipped(self):
        self.series['outputs'][0]['previous_sha256'] = '0' * 64
        self.write_series()
        self.rejected(pattern='first nine patches differ from 0.6.33 hash')

    def test_previous_mixed_with_base_rejected(self):
        self.previous_tree()
        output = next(o for o in self.series['outputs'] if o['path'] == 'core/Connection.c')
        repository = prepare.repository_map(self.series)[output['repository']]
        (self.prepared / output['path']).write_bytes(prepare.git_file(
            self.root / repository['path'], repository['revision'], output['source_path']))
        self.rejected(pattern='partial')

    def test_real_git_replay_and_noop(self):
        before = snapshot(self.root / self.series['repositories'][0]['path'])
        self.assertEqual(prepare.apply(self.root), 'prepared')
        expected = {o['path']: o['prepared_sha256'] for o in self.series['outputs']}
        self.assertEqual(prepare.prepared_digests(self.root), expected)
        once = snapshot(self.root)
        self.assertEqual(prepare.apply(self.root), 'unchanged')
        self.assertEqual(prepare.apply(self.root, check=True), 'checked')
        self.assertEqual(once, snapshot(self.root))
        self.assertEqual(before, snapshot(self.root / self.series['repositories'][0]['path']))

    def test_all_base_publication(self):
        self.base_tree()
        self.assertEqual(prepare.apply(self.root), 'prepared')
        self.assertEqual(prepare.prepared_digests(self.root), prepare.prepared_digests(ROOT))

    def test_check_absent_and_base_never_publishes(self):
        self.rejected(lambda: prepare.apply(self.root, check=True))
        self.base_tree()
        self.rejected(lambda: prepare.apply(self.root, check=True))

    def test_all_four_wrong_revisions(self):
        for repo in self.series['repositories']:
            with self.subTest(repository=repo['name']):
                modified = copy.deepcopy(self.series)
                next(r for r in modified['repositories'] if r['name'] == repo['name'])['revision'] = '0' * 40
                self.write_series(modified)
                self.rejected(pattern='revision mismatch')

    def test_tracked_untracked_ignored_and_frame_header_edits(self):
        core = self.root / self.series['repositories'][1]['path']
        target = core / 'src/VideoDepacketizer.c'
        original = target.read_bytes()
        changed = original.replace(b'frameHeaderSize = 44;', b'frameHeaderSize = 45;', 1)
        self.assertNotEqual(changed, original)
        target.write_bytes(changed)
        self.rejected(pattern='local source edits')
        target.write_bytes(original)
        for name in ('unknown.h', 'ignored.h'):
            if name == 'ignored.h':
                (core / '.git/info').mkdir(exist_ok=True)
                (core / '.git/info/exclude').write_text('ignored.h\n')
            extra = core / name
            extra.write_text('private local edit')
            self.rejected(pattern='local source edits')
            extra.unlink()

    def test_patch_tool_metadata_and_object_hash_rejections(self):
        patch = self.root / prepare.SERIES.parent / self.series['patches'][0]['path']
        original = patch.read_bytes()
        patch.write_bytes(original + b'\n')
        self.rejected(pattern='patch checksum mismatch')
        patch.write_bytes(original)
        tool = self.root / 'tools/moonlight/prepare.py'
        source = tool.read_bytes()
        tool.write_bytes(source + b'\n')
        self.rejected(pattern='tool checksum mismatch')
        tool.write_bytes(source)
        for key in ('base_sha256', 'prepared_sha256'):
            changed = copy.deepcopy(self.series)
            changed['outputs'][0][key] = '0' * 64
            self.write_series(changed)
            self.rejected(pattern='hash')
        self.write_series()
        extra = patch.parent / 'unknown.patch'
        extra.write_text('unknown')
        self.rejected(pattern='directory and series')

    def test_hidden_index_flags_reject_prepare_fetch_and_manifest(self):
        core = self.root / self.series['repositories'][1]['path']
        relative = 'src/PlatformCrypto.c'
        target = core / relative
        original = target.read_bytes()
        self.assertNotIn('core/PlatformCrypto.c', {o['path'] for o in self.series['outputs']})
        for flag, tag in (('assume-unchanged', 'h'), ('skip-worktree', 'S')):
            with self.subTest(flag=flag):
                subprocess.run(['git', '-C', str(core), 'update-index', '--' + flag, relative],
                               check=True, env=prepare.git_environment())
                # Flags alone are rejected, rather than temporarily clearing user hints.
                self.rejected(pattern='source index has assume-unchanged/skip-worktree')
                target.write_bytes(original + b'\n/* actual hidden worktree edit */\n')
                self.assertEqual(prepare.command_text('git', '-C', str(core),
                                                     'diff', '--name-only', 'HEAD'), '')
                self.assertEqual(prepare.command_text('git', '-C', str(core),
                                                     'ls-files', '-v', relative), tag + ' ' + relative)
                indexes = {self.root / r['path'] / '.git/index':
                           (self.root / r['path'] / '.git/index').read_bytes()
                           for r in self.series['repositories']}
                self.rejected(pattern='source index has assume-unchanged/skip-worktree')
                self.rejected(lambda: fetch.fetch(self.root), pattern='source index has')
                with mock.patch.object(manifest, 'ROOT', self.root):
                    self.rejected(lambda: manifest.build_manifest(self.root / headers.PREFIX),
                                  pattern='source index has')
                self.assertEqual(indexes, {path: path.read_bytes() for path in indexes})
                self.assertEqual(target.read_bytes(), original + b'\n/* actual hidden worktree edit */\n')
                self.assertEqual(prepare.command_text('git', '-C', str(core),
                                                     'ls-files', '-v', relative), tag + ' ' + relative)
                target.write_bytes(original)
                subprocess.run(['git', '-C', str(core), 'update-index', '--no-' + flag, relative],
                               check=True, env=prepare.git_environment())

    def test_fetch_hidden_flags_rejected_before_missing_repository_clone(self):
        core = self.root / self.series['repositories'][1]['path']
        subprocess.run(['git', '-C', str(core), 'update-index', '--skip-worktree',
                        'src/PlatformCrypto.c'], check=True, env=prepare.git_environment())
        (core / 'src/PlatformCrypto.c').write_text('private hidden crypto source edit')
        enet = self.root / self.series['repositories'][2]['path']
        enet.rename(self.root / 'unavailable-enet')
        run = subprocess.run
        def refuse_clone(args, *positional, **keywords):
            if args[:2] == ['git', 'clone'] or 'checkout' in args:
                raise AssertionError('fetch attempted mutation before rejecting hidden source flags')
            return run(args, *positional, **keywords)
        with mock.patch.object(subprocess, 'run', refuse_clone):
            self.rejected(lambda: fetch.fetch(self.root), pattern='source index has')

    def test_invalid_metadata_paths(self):
        for value in ('../escape', '/tmp/escape', '.', 'a/../../escape', 'a\\b', 'a//b'):
            changed = copy.deepcopy(self.series)
            changed['outputs'][0]['path'] = value
            self.write_series(changed)
            self.rejected(pattern='unsafe')
        changed = copy.deepcopy(self.series)
        changed['outputs'][1]['path'] = changed['outputs'][0]['path']
        self.write_series(changed)
        self.rejected(pattern='duplicate')

    def test_patch_undeclared_path(self):
        patch = self.root / prepare.SERIES.parent / self.series['patches'][-1]['path']
        patch.write_bytes(patch.read_bytes() + b'\ndiff --git a/unknown.h b/unknown.h\nnew file mode 100644\n--- /dev/null\n+++ b/unknown.h\n@@ -0,0 +1 @@\n+unknown\n')
        self.series['patches'][-1]['sha256'] = prepare.digest(patch)
        self.write_series()
        self.rejected(pattern='outside the declared')

    def test_prepared_unknown_missing_truncated_mixed(self):
        prepare.apply(self.root)
        target = self.prepared / 'core/Connection.c'
        original = target.read_bytes()
        for name in ('stale.h', 'stale.c'):
            extra = self.prepared / name
            extra.write_text('unowned')
            self.rejected(pattern='unknown paths')
            extra.unlink()
        target.write_bytes(original[:15])
        self.rejected(pattern='truncated')
        target.unlink()
        self.rejected(pattern='missing')
        output = next(o for o in self.series['outputs'] if o['path'] == 'core/Connection.c')
        repo = prepare.repository_map(self.series)[output['repository']]
        target.write_bytes(prepare.git_file(self.root / repo['path'], repo['revision'], output['source_path']))
        self.rejected(pattern='partial')
        target.write_bytes(original)
        (self.prepared / 'empty-unknown-dir').mkdir()
        self.rejected(pattern='unknown paths')

    def test_links_fifo_and_parent_escape(self):
        prepare.apply(self.root)
        target = self.prepared / 'core/Connection.c'
        external = self.root / 'external-file'
        external.write_bytes(target.read_bytes())
        target.unlink()
        target.symlink_to(external)
        self.rejected(pattern='symbolic link')
        target.unlink()
        target.symlink_to(self.root / 'absent-target')
        self.rejected(pattern='symbolic link')
        target.unlink()
        os.mkfifo(target)
        self.rejected(pattern='non-regular')
        target.unlink()
        target.write_bytes(external.read_bytes())
        outside = self.root / 'outside-core'
        (self.prepared / 'core').rename(outside)
        (self.prepared / 'core').symlink_to(outside, target_is_directory=True)
        self.rejected(pattern='symbolic link')

    def test_source_symlinks_and_git_identity(self):
        core = self.root / self.series['repositories'][1]['path']
        target = core / 'src/Connection.c'
        external = self.root / 'outside-source'
        external.write_bytes(target.read_bytes())
        target.unlink()
        target.symlink_to(external)
        self.rejected(pattern='symbolic link')
        target.unlink()
        target.write_bytes(external.read_bytes())
        config = core / '.git/config'
        config.write_text('[core]\n repositoryformatversion = 0\n bare = false\n worktree = ' + str(self.root) + '\n')
        self.rejected(pattern='identity mismatch')

    def test_source_parent_symlink_and_patch_symlink(self):
        core = self.root / self.series['repositories'][1]['path']
        outside = self.root / 'external-src'
        (core / 'src').rename(outside)
        (core / 'src').symlink_to(outside, target_is_directory=True)
        self.rejected(pattern='symbolic link')
        (core / 'src').unlink()
        outside.rename(core / 'src')
        patch = self.root / prepare.SERIES.parent / self.series['patches'][0]['path']
        external_patch = self.root / 'external.patch'
        patch.rename(external_patch)
        patch.symlink_to(external_patch)
        self.rejected(pattern='non-regular')

    def test_git_environment_cannot_redirect_fixture_or_index(self):
        sentinel = self.root / 'external-index'
        sentinel.write_bytes(b'do not overwrite')
        source = self.root / self.series['repositories'][0]['path']
        with mock.patch.dict(os.environ, {'GIT_DIR': str(source / '.git'),
                                         'GIT_WORK_TREE': str(ROOT),
                                         'GIT_INDEX_FILE': str(sentinel)}):
            self.assertEqual(prepare.apply(self.root), 'prepared')
            archive = self.root / 'environment-archive'
            checker.archive(source, archive, self.series['repositories'][0]['revision'])
            self.assertEqual(Path(prepare.command_text('git', '-C', str(archive),
                                                       'rev-parse', '--show-toplevel')), archive)
        self.assertEqual(sentinel.read_bytes(), b'do not overwrite')

    def test_symlink_above_fixture_root(self):
        alias = self.root / 'alias'
        real = self.root / 'real'
        real.mkdir()
        alias.symlink_to(real, target_is_directory=True)
        (real / 'child').mkdir()
        with self.assertRaisesRegex(prepare.PrepareError, 'ancestry'):
            prepare.validate_ancestry(alias / 'child', 'missing')
        with self.assertRaisesRegex(prepare.PrepareError, 'ancestry'):
            prepare.validate_ancestry(alias / '..' / 'real' / 'child', 'missing')

    def test_crash_evidence_preserved(self):
        for relative in (prepare.STAGING, prepare.BACKUP):
            path = self.root / relative
            path.mkdir()
            (path / 'recovery-evidence').write_text('keep')
            self.rejected(pattern='incomplete previous')
            shutil.rmtree(path)

    def test_both_publication_rename_failures(self):
        self.base_tree()
        original = Path.rename
        for failure in (1, 2):
            with self.subTest(failure=failure):
                calls = 0
                def rename(path, target):
                    nonlocal calls
                    calls += 1
                    if calls == failure:
                        raise OSError('injected rename failure')
                    return original(path, target)
                with mock.patch.object(Path, 'rename', rename):
                    self.rejected(pattern='injected rename failure')
        self.assertEqual(prepare.apply(self.root), 'prepared')

    def test_absent_publication_failure(self):
        with mock.patch.object(Path, 'rename', side_effect=OSError('injected')):
            self.rejected(pattern='injected')

    def test_rollback_failure_preserves_evidence(self):
        self.base_tree()
        before = snapshot(self.prepared)
        original = Path.rename
        calls = 0
        def rename(path, target):
            nonlocal calls
            calls += 1
            if calls in (2, 3):
                raise OSError('injected publication and rollback')
            return original(path, target)
        with mock.patch.object(Path, 'rename', rename):
            with self.assertRaisesRegex(prepare.PrepareError, 'recovery evidence preserved'):
                prepare.apply(self.root)
        self.assertEqual(before, snapshot(self.root / prepare.BACKUP))
        self.assertTrue((self.root / prepare.STAGING).is_dir())
        self.rejected(pattern='incomplete previous')

    def test_post_publication_validation_failure_rolls_back(self):
        self.base_tree()
        before = snapshot(self.root)
        stage = self.root / prepare.STAGING
        prepare.stage_outputs(self.root, self.series, stage)
        def fail():
            raise OSError('post-publication validation failure')
        with self.assertRaises(OSError):
            prepare.publish_staged(self.root, stage, self.prepared, self.root / prepare.BACKUP, 'base', fail)
        self.assertEqual(before, snapshot(self.root))

    def test_second_classification_unknown_file_cleans_only_owned_stage(self):
        prepare.apply(self.root)
        original = prepare.stage_outputs
        unknown = self.prepared / 'external-unknown.h'
        prepared_before = snapshot(self.prepared)
        def stage_then_external_edit(root, series, stage):
            original(root, series, stage)
            unknown.write_text('preserve external work')
        with mock.patch.object(prepare, 'stage_outputs', stage_then_external_edit):
            with self.assertRaisesRegex(prepare.PrepareError, 'unknown paths'):
                prepare.apply(self.root)
        self.assertEqual(unknown.read_text(), 'preserve external work')
        self.assertFalse(prepare.lexists(self.root / prepare.STAGING))
        self.assertFalse(prepare.lexists(self.root / prepare.BACKUP))
        self.assertEqual(self.sentinel.read_bytes(), b'preserve me')
        unknown.unlink()  # Only the external cause is removed by the fixture owner.
        self.assertEqual(prepared_before, snapshot(self.prepared))
        self.assertEqual(prepare.apply(self.root), 'unchanged')

    def test_second_classification_missing_file_cleans_stage_and_allows_retry(self):
        prepare.apply(self.root)
        original = prepare.stage_outputs
        target = self.prepared / 'core/Connection.c'
        before = target.read_bytes()
        def stage_then_external_removal(root, series, stage):
            original(root, series, stage)
            target.unlink()
        with mock.patch.object(prepare, 'stage_outputs', stage_then_external_removal):
            with self.assertRaisesRegex(prepare.PrepareError, 'missing'):
                prepare.apply(self.root)
        self.assertFalse(target.exists())
        self.assertFalse(prepare.lexists(self.root / prepare.STAGING))
        self.assertFalse(prepare.lexists(self.root / prepare.BACKUP))
        target.write_bytes(before)  # Restore only the externally removed file.
        self.assertEqual(prepare.apply(self.root), 'unchanged')

    def test_second_classification_read_failure_cleans_stage_and_allows_retry(self):
        prepare.apply(self.root)
        before = snapshot(self.root)
        original_stage = prepare.stage_outputs
        original_read = Path.read_bytes
        target = self.prepared / 'core/Connection.c'
        fail_read = False
        def stage_then_read_error(root, series, stage):
            nonlocal fail_read
            original_stage(root, series, stage)
            fail_read = True
        def read_bytes(path):
            nonlocal fail_read
            if fail_read and path == target:
                fail_read = False
                raise PermissionError('injected pre-publication read failure')
            return original_read(path)
        with mock.patch.object(prepare, 'stage_outputs', stage_then_read_error), \
                mock.patch.object(Path, 'read_bytes', read_bytes):
            with self.assertRaisesRegex(PermissionError, 'injected pre-publication read failure'):
                prepare.apply(self.root)
        self.assertEqual(before, snapshot(self.root))
        self.assertEqual(prepare.apply(self.root), 'unchanged')

    def test_fetch_rejects_existing_edits_without_checkout(self):
        target = self.root / self.series['repositories'][2]['path'] / 'untracked.h'
        target.write_text('preserve')
        self.rejected(lambda: fetch.fetch(self.root), pattern='local edits')

    def header_fixture(self):
        prepare.apply(self.root)
        archive = self.root / headers.CURL_ARCHIVE
        archive.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / headers.CURL_ARCHIVE, archive)
        prefix = self.root / headers.PREFIX
        prefix.mkdir(parents=True, exist_ok=True)
        (prefix / '.include.lock').touch()
        return prefix

    def test_complete_header_publication_and_noop(self):
        self.header_fixture()
        self.assertEqual(headers.publish(self.root), 'published')
        self.assertEqual(len(headers.installed_digests(self.root)), 26)
        before = snapshot(self.root)
        self.assertEqual(headers.publish(self.root), 'unchanged')
        self.assertEqual(before, snapshot(self.root))

    def test_headers_unknown_link_and_crash_rejection(self):
        prefix = self.header_fixture()
        headers.publish(self.root)
        extra = prefix / 'include/stale.h'
        extra.write_text('keep')
        self.rejected(lambda: headers.publish(self.root), pattern='unknown installed')
        extra.unlink()
        target = prefix / 'include/Settings.hpp'
        target.unlink()
        target.symlink_to(self.root / 'absent-header')
        self.rejected(lambda: headers.publish(self.root), pattern='symbolic link')
        target.unlink()
        for relative in (headers.STAGING, headers.BACKUP):
            (self.root / relative).mkdir()
            self.rejected(lambda: headers.publish(self.root), pattern='incomplete header')
            (self.root / relative).rmdir()

    def test_header_both_rename_failures_and_sentinels(self):
        prefix = self.header_fixture()
        headers.publish(self.root)
        (prefix / 'include/Settings.hpp').write_text('known old installed header')
        (prefix / 'outside-include.keep').write_text('do not touch')
        original = Path.rename
        for failure in (1, 2):
            calls = 0
            def rename(path, target):
                nonlocal calls
                calls += 1
                if calls == failure:
                    raise OSError('injected header rename')
                return original(path, target)
            with mock.patch.object(Path, 'rename', rename):
                self.rejected(lambda: headers.publish(self.root), pattern='injected header')
        self.assertEqual(headers.publish(self.root), 'published')
        self.assertEqual((prefix / 'outside-include.keep').read_text(), 'do not touch')

    def test_manifest_cache_invalidation(self):
        prefix = self.header_fixture()
        headers.publish(self.root)
        files = [prefix / 'lib' / name for name in
                 ('libgamestream.a', 'libmoonlight-common-c.a', 'libmoonlight-curl.a', 'libenet.a')]
        portlibs = self.root / 'fake-portlibs'
        files += [portlibs / name for name in ('libexpat.a', 'libz.a')]
        files += [self.root / 'third_party/rtc/switch/lib' / name for name in
                  ('libmbedtls.a', 'libmbedx509.a', 'libmbedcrypto.a', 'libeverest.a', 'libp256m.a')]
        files += [self.root / 'tools/rtc/mbedtls-user-config.h', prefix / 'curl-symbols.map']
        for path in files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture dependency')
        (prefix / 'curl-build.json').write_text('{}')
        with mock.patch.object(manifest, 'ROOT', self.root), mock.patch.object(manifest, 'PORTLIBS', portlibs), \
                mock.patch.object(manifest, 'command', return_value='fixture compiler'):
            recorded = manifest.build_manifest(prefix)
            (prefix / 'build-manifest.json').write_text(json.dumps(recorded))
            check.check()
            mutations = [self.root / prepare.SERIES, self.root / 'tools/moonlight/README.md',
                         self.root / 'tools/moonlight/FogMoonlightDiagnostics.h',
                         self.prepared / 'core/Connection.c', prefix / 'include/curl/curl.h',
                         prefix / 'include/gamestream/client.h', prefix / 'lib/libenet.a']
            mutations += [self.root / prepare.SERIES.parent / patch['path'] for patch in self.series['patches']]
            for path in mutations:
                with self.subTest(path=path.relative_to(self.root)):
                    original = path.read_bytes()
                    path.write_bytes(original + b'\n')
                    with self.assertRaises((ValueError, prepare.PrepareError)):
                        check.check()
                    path.write_bytes(original)
            for repository in self.series['repositories']:
                head = self.root / repository['path'] / '.git/HEAD'
                old = head.read_bytes()
                head.write_text('0' * 40 + '\n')
                with self.assertRaises((prepare.PrepareError, subprocess.CalledProcessError)):
                    check.check()
                head.write_bytes(old)
            with mock.patch.object(manifest, 'command', return_value='different compiler'):
                with self.assertRaises(ValueError):
                    check.check()
            check.check()


if __name__ == '__main__':
    unittest.main(verbosity=2)
