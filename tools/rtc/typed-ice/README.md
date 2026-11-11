# Typed ICE SDK candidate (not integrated)

`typed-ice.patch` is an additive overlay on the exact approved RTC series whose
SHA-256 is recorded in `candidate.json`. It is deliberately outside the active
`tools/rtc/patches/` directory. Nothing in this directory publishes SDK artifacts.

Public contract: `rtc::IceDiagnostics rtc::PeerConnection::getIceDiagnostics() const`.
The complete proposed public header is `icediagnostics.hpp`. Fields have the same
numeric types/units as `juice_diagnostics_t`; signed error values stay signed.
Use `availability` first, then `provenance`; zero in an unavailable result is not
an observation. `socketError` additionally requires `socketErrorAvailable`.

Call this potentially blocking getter only on the existing background ICE owner.
An explicit query takes the juice connection lock. A successful query primes a
separate typed cache; neither legacy string queries nor teardown populate it.
After close, values are `Available + FinalRetained`, or `ClosedWithoutSample + None`.
The cache is a last observation, not a fresh sample of the closed transport.
An in-flight query may finish after close; before completion, a closed cache read
returns the earlier sample or `ClosedWithoutSample`; afterwards it returns the
completed observation marked `FinalRetained`. Closed reads and close itself do
not wait for that live query. `ReadFailed` is returned on a failed live read while
the last successful cache remains intact. Libnice is explicitly `NotSupported`.
`Live` describes the query's observation/linearization point, not a guarantee that
the transport remains open when a caller later consumes its immutable copy.

## Reproduce in Ubuntu WSL

From `/home/maxnemoy/source/fogplay_nx`, with a **new** work directory each time:

```sh
python3 -B tools/rtc/typed-ice/verify.py --work /tmp/typed-ice-review-NEW --jobs 4
python3 -B tools/rtc/typed-ice/verify.py --work /tmp/typed-ice-asan-NEW --jobs 3 \
  --sanitizer address --skip-target-syntax
```

The driver archives pinned Git objects and recursively pinned submodules, checks
all approved input/output hashes, applies the approved series then the candidate,
and checks the exact eight-file source delta. It compiles and links the full
pinned host SDK against privately built pinned mbedTLS, then runs six runtime
executables, an exact-production-body libnice branch fixture, and (by default)
syntax checks for all three changed translation units with the Switch toolchain
and actual recipe flags. The ASan mode instruments the SDK and its dependencies
with AddressSanitizer and UndefinedBehaviorSanitizer. A nonzero command stops the
run with `status: failed`, retaining logs and exact argv/exit codes in `results.json`.
The driver does not run the normal shared fetch/build/publication pipeline.

Full libnice integration is not tested (libnice development files are absent).
The branch fixture compiles the actual getter bodies under `USE_NICE=1` with
minimal lifetime scaffolding; it is labeled separately from the real libjuice SDK
tests. Target syntax is not target linking, execution or hardware acceptance.

`generate-overlay.py /tmp/typed-ice-generate-NEW` deterministically regenerates the
overlay and `candidate.json` from the proposed header and exact code insertions
in a fresh fixture. It intentionally rewrites **only these candidate artifacts**.
Normal verification consumes the reviewed overlay; it does not regenerate it.

## Integration after independent approval (integrator only)

Keep the current validator unchanged. It recognizes whole pristine or whole
approved-final source states; the old approved final is not a new base revision.

1. Freeze dependency writers/builds. Keep the complete old series, metadata and
   source backup available. Validate its exact final bytes, pinned revisions,
   declared path set and hidden-index flags with the current approved validation
   logic; reject unrelated, truncated, hidden or partial changes. Do not clear
   index flags automatically and do not reset a dirty dependency checkout.
2. In a private pinned fixture, apply the old series then this overlay. Generate
   one cumulative replacement `0001-libdatachannel.patch` from the **original
   pinned Git base** to these new final bytes. Keep `0002-libjuice.patch` and
   `0003-usrsctp.patch` byte-identical. Update the first repository's patch hash
   and changed output hashes in a new reviewed `series.json`, adding the new
   header with `base_sha256: null`. Keep revisions, base hashes, tool-input hashes
   and every unrelated output unchanged. There are now 39 declared outputs.
3. In private real-Git fixtures, test the unchanged D4 validator with that proposed
   series: pristine apply, exact-new-final no-op, unknown edit rejection, hidden
   flag rejection, partial/truncated states, and failure rollback. The old-final
   tree presented to the new series must be rejected, not accepted as an implicit
   migration state. Review this cumulative patch/metadata pair before publishing.
4. Under the dependency maintenance exclusion, use a separately reviewed,
   exact-hash-guarded transition from old-final to pristine: verify every old
   final byte and back it up; reverse the **saved old approved** patches in child
   then parent order (`0003`, `0002`, `0001`); verify every pristine hash and
   absence of old added files. Only then publish the approved replacement patch
   and metadata and run the unchanged normal applier. Do not run builds midway.
   On any error, stop; restore the complete verified old bytes/series from the
   backup. A crash/partial transition must fail closed on the next validation.
   Never teach D4 to accept arbitrary mixtures or add old-final hashes to an
   expanded permissive acceptance list.
5. Root rebuilds the normal host and Switch dependencies, publishes/validates
   manifests and installed headers/archives, and performs normal regression/link
   gates. Only then connect T2's background producer and central numeric mapper.
   Keep the existing capture clocks/revisions and label final retained data;
   do not query this getter on UI, formatter or teardown paths.

The existing manifest fingerprint recursively includes candidate `.py`, `.cpp`,
`.patch` and `.json` files under `tools/rtc`, so merely adding this isolated review
directory makes old manifests stale. No manifest/recipe was edited or refreshed
here. The integrator must account for final candidate file contents in the normal
rebuild; do not suppress or relax that fingerprint.

Full evidence and acceptance boundaries:
`D:/dev/experements/mts/_review/implementation-v2/TYPED-ICE-SDK-DEVELOPMENT.md`.
