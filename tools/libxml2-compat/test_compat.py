import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("compat", Path(__file__).with_name("compat.py"))
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class SetupTests(unittest.TestCase):
    def test_setup_does_not_download_without_explicit_option(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(compat, "require"), patch.object(compat, "run"), patch.object(compat.urllib.request, "urlopen") as download:
            path = Path(directory)
            args = argparse.Namespace(archive=None, download=False)
            with self.assertRaisesRegex(RuntimeError, "explicit --download"):
                compat.setup(args, path, path)
            download.assert_not_called()

    def test_wrong_archive_rejected_before_extract_or_build(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(compat, "require"), patch.object(compat, "run") as command, patch.object(compat.tarfile, "open") as extract:
            path = Path(directory)
            archive = path / "wrong.tar.gz"
            archive.write_bytes(b"not the pinned source")
            args = argparse.Namespace(archive=archive, download=False)
            with self.assertRaisesRegex(RuntimeError, "Source archive checksum mismatch"):
                compat.setup(args, path, path)
            extract.assert_not_called()
            # The prerequisite pkg-config check is the only command allowed.
            self.assertEqual(command.call_args_list[0].args[0], ["pkg-config", "--exists", "zlib"])
            self.assertEqual(command.call_count, 1)

    def test_recipe_checksum_rejected(self):
        with patch.object(compat, "digest", return_value="wrong"):
            with self.assertRaisesRegex(RuntimeError, "recipe checksum mismatch"):
                compat.recipe_check()

    def test_missing_tool_fails_with_actionable_message(self):
        with patch.object(compat.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, r"Missing required tool: g\+\+"):
                compat.require("g++")

    def test_missing_dependency_is_not_a_skip(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "setup --download"):
                compat.verify(Path(directory), Path(directory))

    def test_tampered_install_is_rejected_before_compiler_runs(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(compat, "run") as command:
            path = Path(directory)
            (path / "install/lib").mkdir(parents=True)
            library = path / "install/lib/libxml2.a"
            library.write_bytes(b"original")
            manifest = {"inputs": compat.fingerprint(), "source_sha256": compat.SOURCE_SHA256,
                        "installed_sha256": compat.install_hashes(path / "install")}
            (path / "manifest.json").write_text(json.dumps(manifest))
            library.write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "integrity check failed"):
                compat.verify(path, path)
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()