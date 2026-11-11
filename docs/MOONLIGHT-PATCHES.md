# Moonlight prepared source patch series

`tools/moonlight/prepare.py` builds a closed 24-file prepared tree from pinned
Git objects and ten ordered patches in `tools/moonlight/patches/series.json`.
It never modifies an upstream worktree. The first eight patches preserve all
C7 frozen bytes, including its default-OFF policy. Patch 0009 records the
0.6.33 same-wait IDR throttle; patch 0010 exactly reverses it to restore prompt
recovery after a new loss. Encryption algorithms, transport selection, MTS
and reference-frame admission are unchanged.

## Pins and frozen provenance

| Repository | Commit |
| --- | --- |
| XITRIX/Moonlight-Switch | `a75fee223914f36ec26b29739d95331307960fa3` |
| XITRIX/moonlight-common-c | `206c79c240c342229555494a8cadda6c11cad479` |
| cgutman/enet | `e261d80db1de7ee92fe4fc45db67887df8b45314` |
| sleepybishop/nanors | `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a` |

The C7 reference is `c7-frozen-20261104T113436Z` from 2026-11-04.
Its `manifest.json` SHA-256 is
`a31f08269724a26a48056453745edabb0021863fe737307132509376ac9eefb5`;
its `source-prepared.tar.gz` SHA-256 is
`bddf2ebc915b0b52802593a11994652744e7b0b418ebd9c442aaa02cad40ad77`.
All 79 snapshot members were verified. Two isolated runs of the frozen legacy
recipe, each intermediate transformation, and the first eight patches reproduce
the same complete 24 files. Seven outputs are unchanged copies and 17 are modified.
The metadata records each output's source repository/path, base and final
SHA-256, every patch SHA-256, and the frozen source/hash of each transformation.
The retained Python recipe modules are historical provenance only; neither
preparation nor the native drivers import them. The old dispatcher is preserved
in the frozen reference. The patches themselves are the active corresponding
source and do not require that external snapshot to run.

Sorted `path + NUL + final-sha256 + LF` records have SHA-256
`47551be611ce395297c367f01e70e60839533b6bdb3b7821bc0be3ad63db784a`.
This remains the mandatory checkpoint after patch 0008 and is again the
final tree after patch 0010. Patch 0009's 0.6.33 bytes are verified as a separate
intermediate checkpoint. Per-file checks, not this aggregate alone, determine
acceptance. Series schema 2 records `frozen_sha256`, `previous_sha256` (0.6.33)
and `prepared_sha256` for every output.

## Ordered transformations and custom APIs

The Switch pin supplies GameStream/crypto sources; the common-c pin supplies
the ten prepared core units. ENet and nanors pins remain validated build inputs.

| Patch | Frozen recipe | Custom surface and visibility | Behavioral coverage |
| --- | --- | --- | --- |
| 0001-probe-and-termination | prepare.py, first transformation block | GameStream acknowledgement/identity/crypto fixes; internal synchronous termination shim calling local `FogMoonlightTermination` API | command acknowledgements, termination origin, session, TLS |
| 0002-production | production.py | Exported `http_pin_certificate(const Data&)`, `SERVER_DATA.urlAddress`; SPKI policy and raw-host/IPv6 URL separation | TLS pin/impostor/identity rotation, session and command tests |
| 0003-command-diagnostics | prepare.py, command block | Internal numeric HTTP/XML/command observations via local compatibility API | command acknowledgements, XML diagnostics, log integration |
| 0004-core-diagnostics | diagnostics.py | Local public `FogMoonlightDiagnostics.h` snapshot and private `FogMoonlightDiagnosticsInternal.h` hooks | core queue, RTP/FEC, receive, socket diagnostics |
| 0005-idr-recovery | recovery.py | Internal `FogIdrRecoveryPollDecoder`; local `FogMoonlightRecovery.[ch]` recovery API and partial-IDR guard | IDR recovery, queue wake/completion, session |
| 0006-idr-diagnostics | idr_diagnostics.py | Private IDR/FEC/thread journal instrumentation and pinned frame-header policy | RTP/FEC, IDR, receive and log tests; pinned source mutation rejection |
| 0007-rtsp-transport | transport.py | Internal `FogMoonlightConnectTcpSocket` / `FogMoonlightPollSockets`; local termination-origin API | RTSP/socket cancellation and termination origin |
| 0008-encryption | encryption.py, frozen C7 | Required-mode fail-closed negotiation; public `FOG_MOONLIGHT_ENCRYPTION_CONFLICT` via local diagnostics header; internal selection/decrypt observations | known vectors/tamper, full RTSP/SDP matrix, actual video/audio boundaries, default/control/opt-in sessions |
| 0009-same-wait-idr-gate | Historical 0.6.33 experiment | Intermediate per-wait throttle; exact original patch retained | 0.6.33 intermediate checkpoint and migration |
| 0010-restore-loss-idr-retry | 0.6.34 rollback | Exact inverse of 0009; next complete frame after each new loss requests immediately, while no-new-loss retries retain the 500 ms fallback | repeated holes and lost requested IDR within the same wait, decoder refresh, deadline and reference safety |

`compat/Settings.hpp` and `compat/Diagnostics.hpp` (`moonlight_diagnostics`) are
local exported compatibility APIs, not upstream prepared files. The local
diagnostics/recovery/termination C units and their headers are included in the
build tool fingerprint. The final tree no longer uses 0009's request-generation
API, which is removed from the local recovery unit. Exact legacy SHA-256 values
for the first eight rows are retained in `series.json`; both later patches
record explicit post-frozen provenance and retain the original 0009 bytes.

## Validation and publication

`fetch.sh` materializes absent pins through `fetch.py`. Every existing worktree
must already have the correct revision and no local tracked, untracked, or
ignored edits. Wrong/dirty existing trees are rejected before any clone or
checkout; the helper never resets or cleans them. Empty submodule directories
may be populated. `prepare.py` also verifies actual Git worktree identity,
all four pins, input hashes, the active loader checksum, patch inventory and
complete output inventory. Caller Git environment redirects are discarded.
Tracked index entries marked `assume-unchanged` or `skip-worktree` are rejected
in both fetch preflight and preparation/cache validation, even if their bytes
currently match the pin. These flags can hide actual edits from `git diff`,
including in compiled core files outside the prepared set such as
`PlatformCrypto.c`. Validation does not clear flags or overwrite user files.

Preparation locks `.prepare.lock` and builds `.prepared.staging` beside
`prepared`. Exact final bytes yield a no-op preserving file mtimes. An absent
or complete all-base tree can be published. An exact all-0.6.33 tree is also
a recognized migration input; check-only mode still requires final bytes.
An exact all-frozen C7 tree already equals the final bytes and is a no-op.
The staged replay must match all 24 frozen hashes after patch 0008, all 24
previous hashes after patch 0009 and all 24 final hashes after patch 0010. Partial, changed, missing, unknown,
non-regular and linked paths (including dangling links and linked ancestors)
are rejected without deletion. Empty unknown directories are also rejected.

Publication renames the old tree to `.prepared.backup`, then renames the staged
tree to `prepared`, and validates it before removing the backup. Ordinary I/O
errors restore the prior tree. If rollback fails, the error identifies the
stage, destination and backup and preserves recovery evidence. Any leftover
stage/backup makes a later run fail explicitly. There is **no automatic
power-loss recovery** or atomic-directory-exchange guarantee. Inspect the named
paths and preserve the prior tree before manually resolving an interrupted run;
never use a blanket reset/delete to bypass an unknown-state error.
If the second live-tree classification fails before publication starts (for
example a concurrent unknown/missing file or a read error), only this run's
new staging tree is removed. The live tree/user edit is preserved, and retry
works after the external cause is resolved. This cleanup does not wrap actual
publication: interrupted-publish and failed-rollback evidence is still retained.

`headers.py` applies the same publication protocol to the generated private
`third_party/moonlight/switch/include` tree. The closed set contains 26 headers:
four GameStream, four crypto, six root-level headers and twelve private curl
headers read directly from the checksum-verified curl 8.15.0 archive. Unknown
existing paths, links and interrupted header transactions fail. Known regular
generated headers can be refreshed during an explicit build. Unrelated prefix
files and global SDK/portlibs are outside this publisher's scope. Both build
entry points preflight the tree; wildcard header copying is removed. Do not run
preparation/builds concurrently with consumers or other builds.

Manifest schema 2 records all prepared hashes, all exported-header hashes,
source revisions and clean state, the complete tools fingerprint, compiler,
archives, curl metadata/symbol map, and shared dependencies/configuration.
`check.py` recomputes and compares the entire manifest and expected include
contents. Old manifests fail and require the normal integrator-scheduled build.
The verified curl archive must remain available for include validation.

## Verification

```sh
python3 -B tools/moonlight/prepare.py --check
python3 -B tools/moonlight/check-source-patches.py
python3 -B tools/moonlight/tests/patch_series_test.py
bash tools/moonlight/test.sh
```

The checker uses private detached Git metadata/indexes with read-only object
alternates and asserts the temporary worktree identity; it never points a
temporary `.git` file at live submodule metadata. Failure tests use these same
real Git fixtures and production functions, including both publication renames,
rollback failure, crash evidence, source/prepared tampering and manifest
invalidation. Native C/C++ fixtures and extraction anchors are preserved;
test.sh preflights once, then runs all existing behavior tests. Each Python
driver also validates prepared inputs when invoked independently.

Switch dependency/full-link builds, refreshed installed manifests, the full
application suite and device/network C7 acceptance are separate integration
gates. Source equivalence and native tests do not establish hardware behavior.
