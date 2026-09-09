# Phase 9 roadmap

Phase 9 begins after the bounded Phase 8 immutable core-session milestone. The Phase 8 P8-C candidate was deliberately not entered: the existing genuine multi-thread kernel-core sibling stops in `snapshot_sibling_worker()` without a stable compiler-owned local or formal parameter that could prove thread-scoped source-value ownership. Adding a variable solely to manufacture that evidence would be metadata/test farming rather than a new post-mortem capability.

The Phase 9 architectural hypothesis is **relocated post-mortem artifact ownership**. A core snapshot may be captured on a build machine, CI runner, container, or another host whose `NT_FILE` pathnames do not exist on the inspection machine. The debugger must preserve those recorded paths as immutable crash evidence while allowing the user to explicitly identify where the corresponding runtime and debug artifacts live locally. It must not guess by basename, scan the host filesystem, or rewrite `CoreSnapshot` evidence.

## P9-A: explicit core module-path substitution — complete

Completed executable capability:

- `SnapshotModulePathResolver` owns explicit recorded-prefix -> local-prefix substitutions with lexical normalization, path-component-aware matching, longest-prefix precedence, and same-prefix replacement; `/build` cannot accidentally match `/building`;
- substitutions are host-side artifact lookup policy only. `CoreSnapshot`, `NT_FILE` mappings, `SnapshotInspectionFrameContext::module_path`, resolved symbol/source ownership, and printed local-value ownership retain the exact pathname recorded in the core;
- `SnapshotModuleAddress` separates immutable `module_path` from resolved `module_file_path`; PIE/ET_DYN load-bias calculation still uses the recorded `NT_FILE` mapping family, while runtime ELF and `.eh_frame` readers open only the explicitly resolved host file;
- `CoreInspectionSession` owns one resolver for its entire lifetime. Initial crash-thread CFI construction, immutable thread switching, symbol/source rendering, and caller source-value inspection all use the same mapping policy rather than separate ad-hoc file lookups;
- `mdbg-core` accepts repeatable startup mappings as `--substitute-module-path <recorded-prefix> <local-prefix>` before the core path; malformed arguments fail before the read-only session starts and no interactive command can mutate snapshot evidence;
- real PIE and non-PIE kernel cores are copied into a same-width variant whose recorded fixture module pathname is replaced by a nonexistent pathname. Without a mapping, caller recovery and the historical `transformed` value remain unavailable; with an explicit fake-recorded -> real-local mapping, the same `mdbg-core` subprocess recovers caller frame 1 and `transformed = 0x458a30bf63ac1619` under both permanent GCC and Clang-large lanes;
- the mapped workflow additionally requires user-visible frame and value output to retain the fake pathname recorded in the modified core, directly proving that local artifact location has not replaced immutable crash identity.

## P9-B: explicit GNU separate-debug-file ownership — complete

Completed executable capability:

- runtime-module identity and debug-information identity are separate. `NT_FILE` ownership, PIE load bias, and `.eh_frame` CFI continue to use the runtime ELF; symbol tables, line tables, and snapshot DWARF local-value inspection may use one explicitly supplied companion debug file without changing recorded module ownership;
- `SnapshotModulePathResolver` accepts an exact recorded-module -> local-debug-file mapping and validates it against the resolved runtime module before debug information is consumed. There is no basename search, sibling-directory probing, build-id scan, or debuginfod fallback;
- the resolved runtime ELF must contain one well-formed `.gnu_debuglink`. Its filename must equal the explicitly supplied companion basename and its standard GNU CRC32 must match the full companion bytes; missing links, malformed section metadata, filename mismatch, unavailable companions, and CRC mismatch fail closed;
- `mdbg-core` exposes repeatable `--debug-file <recorded-module> <local-debug-file>` startup mappings alongside module-prefix substitutions. Both policies remain host-side, read-only artifact lookup configuration;
- genuine toolchain artifacts are produced in integration with `objcopy --only-keep-debug`, `--strip-debug`, and `--add-gnu-debuglink`. The stripped runtime ELF still recovers the caller from its own `.eh_frame` while source-value evidence is absent without a companion; supplying the matching debug file restores module-qualified symbols/source and historical `transformed = 0x458a30bf63ac1619`;
- a corrupted same-basename companion is rejected by GNU debuglink CRC identity rather than accepted because its filename looks plausible;
- the full workflow runs on real PIE and non-PIE kernel cores under both permanent GCC and Clang-large lanes.

Phase 9 deliberately does not claim automatic build-id lookup, debuginfod, generic debug directories, container-root reconstruction, or arbitrary filesystem search. Those are distinct artifact-resolution models and should only be added when a real captured-artifact workflow demonstrates that explicit runtime-path substitution plus explicit GNU debug-file ownership cannot express the required evidence chain.

## Phase 9 bounded milestone — complete

P9-A and P9-B cover the two concrete relocated-artifact failures demonstrated so far: the runtime module moved, and the runtime/debug information split into separately stored artifacts. Continuing with packaging variants without a new failing workflow would be resolver farming, so the relocated-artifact milestone is sealed here.

The next architectural hypothesis is **omitted file-backed core memory reconstruction**. `CoreSnapshot::read_memory()` currently requires requested bytes to be physically captured in an ELF core `PT_LOAD`; Linux core-dump policy can omit clean file-backed pages even when `NT_FILE` plus an explicitly owned runtime artifact can identify the original bytes. A future phase must begin with a genuine kernel core that exhibits that omission and prove exact file-offset/address ownership before any fallback is implemented. It must not silently substitute host-file bytes for anonymous, dirty, or otherwise snapshot-owned memory.

## Selection rule

Choose the smallest real post-mortem workflow that advances immutable evidence recovery. Preserve core-recorded identity separately from host artifact location. Do not scan by basename, enumerate packaging variants, add generic search paths without ownership evidence, or reuse live `/proc` state. Any future host-artifact fallback must be explicit or identity-anchored and must never override bytes actually captured by the core.
