#!/usr/bin/env python3
"""Fail-closed private candidate replay, host build/link/tests and target syntax.

Usage: python3 -B tools/rtc/typed-ice/verify.py --work /tmp/typed-ice-review-NEW
Never calls fetch.sh/build.sh/manifest.py or writes shared sources/install outputs.
The fresh work path is retained for independent review, including all logs.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from fixture import command, prepare, sha

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def method(text, signature):
    start = text.index(signature)
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--sanitizer', choices=['none', 'address', 'thread'], default='none')
    parser.add_argument('--skip-target-syntax', action='store_true')
    args = parser.parse_args()
    work = args.work.resolve()
    # Enforce isolation independently of whatever path the caller passes.
    if work == ROOT or ROOT in work.parents or work in ROOT.parents:
        raise RuntimeError('Work must be outside repository and its ancestors')
    spec = json.loads((HERE / 'candidate.json').read_text())
    assert sha(HERE / 'typed-ice.patch') == spec['overlay_sha256']
    sdk, baseline = prepare(ROOT, work)
    logs = work / 'logs'
    logs.mkdir()
    results = {'work': str(work), 'sanitizer': args.sanitizer, 'commands': [],
               'candidate': spec, 'status': 'running'}

    def run(name, argv, timeout=300, env=None):
        print(name, flush=True)
        start = time.monotonic()
        with (logs / (name + '.log')).open('w') as log:
            log.write('ARGV ' + repr([str(a) for a in argv]) + '\n')
            log.flush()
            try:
                proc = subprocess.run([str(a) for a in argv], cwd=work, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=timeout, env=env)
                code = proc.returncode
            except subprocess.TimeoutExpired:
                code = 124
        results['commands'].append({'name': name, 'argv': [str(a) for a in argv],
            'exit_code': code, 'seconds': round(time.monotonic() - start, 3),
            'log': str(logs / (name + '.log'))})
        (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        if code:
            raise RuntimeError(f'{name} failed: exit {code}; {logs / (name + ".log")}')

    try:
        original = {name: (sdk / name).read_text() for name in
                    ['src/impl/icetransport.cpp', 'src/impl/peerconnection.cpp', 'src/peerconnection.cpp']}
        command('git', 'apply', '--check', HERE / 'typed-ice.patch', cwd=sdk)
        command('git', 'apply', HERE / 'typed-ice.patch', cwd=sdk)
        current = {str(p.relative_to(sdk)): sha(p) for p in sdk.rglob('*') if p.is_file()}
        changed = {name for name in current.keys() | baseline['baseline_files'].keys()
                   if current.get(name) != baseline['baseline_files'].get(name)}
        assert changed == set(spec['files']), changed
        for name, item in spec['files'].items():
            assert baseline['baseline_files'].get(name) == item['approved_sha256'], name
            assert current[name] == item['candidate_sha256'], name
        assert (sdk / 'include/rtc/icediagnostics.hpp').read_bytes() == (HERE / 'icediagnostics.hpp').read_bytes()
        # Compiler-visible files outside this additive overlay are EXACTLY baseline.
        # Explicitly prove old APIs/close code have identical function bytes.
        for name, signatures in {
            'src/impl/icetransport.cpp': ['string IceTransport::iceDiagnostics() const'],
            'src/impl/peerconnection.cpp': ['string PeerConnection::iceDiagnostics() const',
                                           'void PeerConnection::closeTransports()'],
            'src/peerconnection.cpp': ['string PeerConnection::iceDiagnostics() const']}.items():
            for signature in signatures:
                assert method(original[name], signature) == method((sdk / name).read_text(), signature)
        results['source_parity'] = 'PASS: exact eight-file delta; legacy methods and teardown byte-identical'
        results['changed_files'] = sorted(changed)

        cmake = work / 'tools/rtc/CMakeLists.txt'
        cmake.write_text(cmake.read_text() + f'''
add_executable(typed-ice-test "{HERE / 'typed-ice-test.cpp'}")
target_compile_options(typed-ice-test PRIVATE -UNDEBUG)
target_include_directories(typed-ice-test PRIVATE "${{RTC_ROOT}}/third_party/rtc/libdatachannel/deps/libjuice/include")
target_link_libraries(typed-ice-test PRIVATE datachannel-static)
target_link_options(typed-ice-test PRIVATE -Wl,--wrap=juice_get_diagnostics -Wl,--wrap=juice_get_udp_poll_diagnostics)
''')
        build = work / 'build'
        config = ['cmake', '-S', cmake.parent, '-B', build, '-DCMAKE_BUILD_TYPE=Debug',
                  '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
        if args.sanitizer != 'none':
            flags = '-fsanitize=' + ('address,undefined' if args.sanitizer == 'address' else 'thread') + ' -fno-omit-frame-pointer'
            config += ['-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
                       '-DCMAKE_EXE_LINKER_FLAGS=' + flags]
        run('host-configure', config)
        targets = ['typed-ice-test', 'rtc-ice-diagnostics-test', 'rtc-host-loopback',
                   'rtc-udp-budget-test', 'rtc-dtls-diagnostics-test', 'rtc-media-send-diagnostics-test']
        run('host-build', ['cmake', '--build', build, '--target', *targets, '-j', args.jobs], timeout=900)
        for target in targets:
            run(target, [build / target], timeout=60)

        # USE_NICE branch coverage uses exact production getter bodies; this is
        # a compile/semantics fixture, NOT a full libnice integration build.
        nice = work / 'nice-unavailable.cpp'
        nice.write_text('''#include <rtc/icediagnostics.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <cassert>
#define USE_NICE 1
namespace rtc::impl {
struct IceTransport { IceDiagnostics getIceDiagnostics() const; IceUdpPollDiagnostics getIceUdpPollDiagnostics() const; };
struct PeerConnection {
 enum class State { New, Closed };
 mutable std::mutex mTypedIceQueryMutex, mTypedIceCacheMutex;
 mutable IceDiagnostics mFinalTypedIceDiagnostics;
 mutable IceUdpPollDiagnostics mFinalUdpPollDiagnostics;
 std::atomic<State> state{State::New};
 std::shared_ptr<IceTransport> transport;
 auto getIceTransport() const { return transport; }
 IceDiagnostics getIceDiagnostics() const;
 IceUdpPollDiagnostics getIceUdpPollDiagnostics() const;
};
''' + method((sdk / 'src/impl/icetransport.cpp').read_text(),
              'IceDiagnostics IceTransport::getIceDiagnostics() const') + '\n' +
            method((sdk / 'src/impl/peerconnection.cpp').read_text(),
                   'IceDiagnostics PeerConnection::getIceDiagnostics() const') + '\n' +
            method((sdk / 'src/impl/icetransport.cpp').read_text(),
                   'IceUdpPollDiagnostics IceTransport::getIceUdpPollDiagnostics() const') + '\n' +
            method((sdk / 'src/impl/peerconnection.cpp').read_text(),
                   'IceUdpPollDiagnostics PeerConnection::getIceUdpPollDiagnostics() const') + '''
}
int main() {
 rtc::impl::PeerConnection p;
 for (int stage=0; stage<3; ++stage) {
  auto d=p.getIceDiagnostics();
  assert(d.backend==rtc::IceDiagnostics::Backend::LibNice);
  assert(d.availability==rtc::IceDiagnostics::Availability::NotSupported);
  assert(d.provenance==rtc::IceDiagnostics::Provenance::None);
  auto rich=p.getIceUdpPollDiagnostics();
  assert(rich.ice.backend==rtc::IceDiagnostics::Backend::LibNice);
  assert(rich.ice.availability==rtc::IceDiagnostics::Availability::NotSupported);
  assert(!rich.clockAvailable && rich.phaseSampleEvery==0 && rich.phaseSampledBatches==0);
  assert(rich.phaseRecvTotalUs==0 && rich.eventsTotal==0);
  p.transport=std::make_shared<rtc::impl::IceTransport>();
  if(stage==1) p.state=rtc::impl::PeerConnection::State::Closed;
 }
}
''')
        run('nice-branch-compile', ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                  '-I' + str(sdk / 'include'), nice, '-pthread', '-o', work / 'nice-unavailable'])
        run('nice-branch-test', [work / 'nice-unavailable'])

        if not args.skip_target_syntax:
            compiler = Path('/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++')
            if not compiler.is_file():
                raise RuntimeError('Target compiler unavailable; explicitly opt out with --skip-target-syntax')
            target_build = work / 'build-switch'
            env = os.environ.copy()
            env['DEVKITPRO'] = '/opt/devkitpro'
            run('target-configure', ['cmake', '-S', cmake.parent, '-B', target_build,
                '-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake',
                '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'], env=env)
            import shlex
            commands = json.loads((target_build / 'compile_commands.json').read_text())
            # Actual target definitions/includes from the private production recipe.
            for name in ['src/impl/icetransport.cpp', 'src/impl/peerconnection.cpp', 'src/peerconnection.cpp',
                         'deps/libjuice/src/conn_poll.c', 'deps/libjuice/src/juice.c']:
                row = next(c for c in commands if c['file'] == str(sdk / name)
                           and ('datachannel-static.dir' in c['command'] or name.endswith('.c')))
                argv = shlex.split(row['command'])
                output = argv.index('-o')
                del argv[output:output + 2]
                argv.remove('-c')
                argv += ['-fsyntax-only']
                run('target-syntax-' + name.replace('/', '-'), argv, env=env)
        results['status'] = 'passed'
    except Exception as error:
        results['status'] = 'failed'
        results['error'] = str(error)
        raise
    finally:
        (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS:', work / 'results.json', flush=True)


if __name__ == '__main__':
    main()
