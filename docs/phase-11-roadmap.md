# Phase 11 roadmap

Phase 11 begins after the bounded Phase 10 omitted-memory provider milestone. Phase 10 established `read_snapshot_memory()` as an explicit read-only inspection layer that prefers immutable bytes captured in the core and only reconstructs a wholly omitted, unambiguous file-backed range from an explicitly owned runtime artifact with provenance.

The Phase 11 architectural hypothesis is **provenance-aware higher-level snapshot memory consumption**. Artifact fallback is not a new global truth source: each higher-level consumer must have a concrete compiler- or workflow-produced need for omitted file-backed bytes, preserve recorded module identity, and continue to prefer captured core evidence whenever the requested bytes are present there.

## P11-A: artifact-backed static scalar snapshot local — complete

Completed executable capability:

- the existing snapshot caller local-value evaluator accepts the compiler-produced x86-64 `DW_OP_addr` location form for bounded scalar values in a recovered caller frame;
- a real function-scope `static const uint64_t` in the optimized formal-parameter fixture produces the demonstrated location shape under both permanent GCC and Clang-large lanes;
- integration generates a genuine Linux kernel core with `coredump_filter=0x1`, so the clean file-backed scalar bytes are omitted while `NT_FILE` ownership remains available;
- the evaluator resolves the recorded module/load bias, requests exactly the scalar width through `read_snapshot_memory()`, and reports `SnapshotRuntimeArtifact` provenance including recorded module identity, resolved host artifact and file offset;
- scalar widths remain bounded to the existing at-most-eight-byte value model, unsupported location expressions remain explicit failures, and the historical `DW_OP_breg3` caller-register path remains distinct rather than being rewritten around artifact fallback;
- `mdbg-core print` renders the same immutable value and provenance rather than presenting reconstructed artifact bytes as physically captured core evidence.

P11-A does not enumerate new DWARF opcodes or globally route snapshot CFI/stack reads through host artifacts. It promotes one compiler-proven source-value consumer across the Phase 10 provider boundary.

## P11-B: artifact-backed aggregate snapshot local — complete

Completed executable capability:

- the same compiler-produced `DW_OP_addr` memory form now materializes the existing bounded structure value model instead of rejecting all `LocalValueKind::Structure` values at the snapshot boundary;
- a real function-scope `static const` structure containing two 64-bit integer members is emitted by both GCC and Clang in the formal-parameter fixture and reaches the demonstrated snapshot location path;
- the evaluator reads the complete owned aggregate range through `read_snapshot_memory()`, decodes only the already-supported bounded member layout, and returns the existing `LocalScalarValue` structure/member representation. No new aggregate type system or expression interpreter is introduced;
- with `coredump_filter=0x1`, the aggregate is reconstructed from the explicitly owned runtime artifact and reports `SnapshotRuntimeArtifact` provenance with immutable recorded module ownership;
- with `coredump_filter=0x5`, the same file-backed aggregate bytes are physically captured by the kernel core. The higher-level consumer returns identical member values but reports `SnapshotCoreMemory`, proving that captured core evidence still dominates even after artifact fallback is available;
- `DW_OP_breg3` remains scalar-only, scalar `DW_OP_addr` width bounds remain unchanged, structure size remains inside the existing bounded aggregate model, and unsupported/malformed location or ownership states continue to fail closed.

P11-B is a provenance-consumer milestone, not a second DWARF-opcode milestone: it deliberately reuses the same compiler-proven `DW_OP_addr` and the same Phase 10 provider while testing a materially different higher-level value shape and evidence-source decision.

## Phase 11 bounded milestone — complete

P11-A and P11-B establish that higher-level immutable source-value inspection can consume reconstructed file-backed evidence without changing the authority model: captured core bytes win, omitted file-backed bytes may be reconstructed only through explicit ownership, and the returned value carries its provenance. Adding another static scalar width, another structure member count, or another coredump-filter combination without a distinct failing consumer would be provenance-consumer farming, so the current compiler-proven Phase 11 milestone is sealed here.

## Phase 12 promotion: post-mortem source-context presentation — next architectural hypothesis

The immutable core session can already resolve snapshot frames to module-qualified symbols and `file:line`, select threads/frames, print recovered values and inspect provenance-aware memory. The live debugger additionally has a bounded source-context presentation layer, but `mdbg-core` currently stops at source coordinates and cannot show the corresponding source excerpt for the selected immutable frame.

Phase 12 should begin only with a real core-session workflow that has a resolvable source row but lacks usable source context. The first slice should reuse immutable module/debug ownership and an explicit source-path policy rather than scanning the filesystem or treating host source files as core evidence. Acceptance must keep snapshot execution state read-only, distinguish recorded debug/source identity from local presentation paths, preserve deterministic failure when source artifacts are unavailable or ambiguous, and prove the behavior with a genuine kernel core under both compiler lanes.

## Selection rule

Choose the smallest real immutable-debugging workflow whose higher-level result is still missing despite the Phase 10 provider and Phase 11 consumer contract. Do not enumerate additional DWARF opcodes, scalar widths, aggregate shapes, provenance labels, or coredump-filter variants without a separate failing workflow. Reconstructed host artifacts remain explicit evidence sources; they never silently replace bytes captured by the core.
