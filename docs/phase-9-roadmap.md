# Phase 9 roadmap

Phase 9 begins after the bounded Phase 8 immutable core-session milestone. The Phase 8 P8-C candidate was deliberately not entered: the existing genuine multi-thread kernel-core sibling stops in `snapshot_sibling_worker()` without a stable compiler-owned local or formal parameter that could prove thread-scoped source-value ownership. Adding a variable solely to manufacture that evidence would be metadata/test farming rather than a new post-mortem capability.

The next architectural hypothesis is **relocated post-mortem artifact ownership**. A core snapshot may be captured on a build machine, CI runner, container, or another host whose `NT_FILE` pathnames do not exist on the inspection machine. The debugger must preserve those recorded paths as immutable crash evidence while allowing the user to explicitly identify where the corresponding ELF/DWARF artifacts live locally. It must not guess by basename, scan the host filesystem, or rewrite `CoreSnapshot` evidence.

## P9-A: explicit core module-path substitution — complete

Completed executable capability:

- `SnapshotModulePathResolver` owns explicit recorded-prefix -> local-prefix substitutions with lexical normalization, path-component-aware matching, longest-prefix precedence, and same-prefix replacement; `/build` cannot accidentally match `/building`;
- substitutions are host-side artifact lookup policy only. `CoreSnapshot`, `NT_FILE` mappings, `SnapshotInspectionFrameContext::module_path`, resolved symbol/source ownership, and printed local-value ownership retain the exact pathname recorded in the core;
- `SnapshotModuleAddress` separates immutable `module_path` from resolved `module_file_path`; PIE/ET_DYN load-bias calculation still uses the recorded `NT_FILE` mapping family, while `ElfFile`, `.eh_frame`, line-table, and DWARF local-value readers open only the explicitly resolved host file;
- `CoreInspectionSession` owns one resolver for its entire lifetime. Initial crash-thread CFI construction, immutable thread switching, symbol/source rendering, and caller source-value inspection all use the same mapping policy rather than separate ad-hoc file lookups;
- `mdbg-core` accepts repeatable startup mappings as `--substitute-module-path <recorded-prefix> <local-prefix>` before the core path; malformed arguments fail before the read-only session starts and no interactive command can mutate snapshot evidence;
- real PIE and non-PIE kernel cores are copied into a same-width variant whose recorded fixture module pathname is replaced by a nonexistent pathname. Without a mapping, caller recovery and the historical `transformed` value remain unavailable; with an explicit fake-recorded -> real-local mapping, the same `mdbg-core` subprocess recovers caller frame 1 and `transformed = 0x458a30bf63ac1619` under both permanent GCC and Clang-large lanes;
- the mapped workflow additionally requires user-visible frame and value output to retain the fake pathname recorded in the modified core, directly proving that local artifact location has not replaced immutable crash identity.

P9-A does not claim debug-file discovery, build-id lookup, debuginfod, separate `.debug` file ownership, container-root reconstruction, or arbitrary filesystem search. Those are distinct artifact-resolution models and require their own concrete failing workflows.

## Current frontier

Phase 9 is the current post-mortem frontier, but no second slice should be invented merely to extend the resolver. The next milestone must start from a real core/artifact workflow that cannot be expressed by explicit module-prefix substitution — for example a genuinely separate debug-information artifact or build-id-owned lookup — and must identify exactly which evidence establishes module/debug-file identity before implementation begins.

## Selection rule

Choose the smallest real relocated-artifact workflow that advances immutable post-mortem inspection. Preserve core-recorded identity separately from host artifact location. Do not scan by basename, enumerate packaging variants, add generic search paths without ownership evidence, or reuse live `/proc` state. Every new artifact resolver must be explicit or cryptographically/build-id anchored and must remain read-only by construction.
