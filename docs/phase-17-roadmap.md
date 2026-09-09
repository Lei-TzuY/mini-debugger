# Phase 17 Roadmap — compiler-proven XMM source-value recovery

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 17 connects immutable per-thread `NT_FPREGSET` state to the existing DWARF source-value evaluator. The completed slice is intentionally compiler- and artifact-proven rather than a generic SIMD register browser.

## Completed executable slice

- an optimized deterministic fixture produces a real `double xmm_value` whose active DWARF location is `DW_OP_reg17 (xmm0)` under both permanent GCC and Clang-large lanes;
- the normal GitHub Actions core-dump workflow produces a genuine Linux core containing distinct crash-thread and sibling-thread FPREGSET state;
- frame-zero snapshot inspection recognizes bounded 4/8-byte `DW_ATE_float` scalar types and routes the compiler-proven XMM0 location through the selected core thread's existing `CoreFloatingPointState` provider;
- `LocalValueKind::Floating` preserves floating-point type identity instead of silently treating the recovered IEEE-754 bits as an integer;
- `mdbg-core print` renders 4-byte and 8-byte floating scalars from their recovered bit patterns using deterministic scalar precision;
- selecting the sibling core thread changes the XMM-backed provider and recovers its independently seeded value, proving per-thread ownership rather than a process-global register cache;
- the shared local-value parser accepts `DW_FORM_udata` only because the Clang-produced artifact uses that form in the same demonstrated source-value scenario; the parser still rejects unproven forms explicitly;
- missing FPREGSET state, non-frame-zero XMM ownership, unsupported XMM register numbers, non-4/8-byte floating widths, and unsupported location expressions continue to fail closed;
- CI permanently validates the genuine-core oracle, direct source-value integration, and CLI presentation in both GCC and Clang-large lanes.

The demonstrated values are `1234.25` for the crashing thread and `9876.5` for the sibling. Their distinct values are part of the ownership evidence.

## Explicit boundary

Phase 17 does **not** infer historical caller XMM values. The immutable core records only the selected thread's crash-time machine state, and the current CFI model does not reconstruct caller-saved SIMD registers. Expanding frame-zero support to arbitrary XMM registers, vector widths, AVX/YMM/ZMM state, or speculative DWARF forms without a compiler-produced failing scenario would be variant farming rather than architectural progress.

## Phase 18 promotion — typed post-mortem pointer dereference

The next architectural frontier is source-level navigation through an already recovered pointer value, not more SIMD enumeration.

The repository already has bounded DWARF pointer typing, selected-frame source-value recovery, immutable snapshot memory reads, runtime-artifact fallback with provenance, and module/debug-file remapping. What is missing is one controlled operation that composes those layers instead of forcing the user to copy a raw pointer into the unrelated `x` command.

The first Phase 18 slice should start from a genuine compiler-produced pointer local in a core snapshot and prove all of the following:

1. recover the pointer through the existing typed local-value evaluator rather than a second DIE walker;
2. retain enough pointee type information to permit exactly one dereference of a compiler-proven 1–8 byte signed/unsigned scalar pointee;
3. read the pointee through `read_snapshot_memory()` so immutable core bytes and explicit runtime-artifact fallback keep their existing provenance and ambiguity rules;
4. expose the dereferenced source value through the core inspection API and `mdbg-core` without accepting arbitrary expression syntax;
5. preserve selected-thread/frame ownership and reject stale or incompatible inspection contexts;
6. fail closed for null/unmapped pointers, unavailable/ambiguous memory, aggregates, arrays, pointer-to-pointer expansion, unsupported pointees, or any second dereference.

Do not turn Phase 18 into a general C/C++ expression evaluator. Further pointer/aggregate navigation requires a new concrete compiler- and artifact-proven failing workflow.

## Selection rule

Choose a genuine core/source scenario whose existing typed pointer value is recoverable but whose pointee cannot yet be inspected through the source-value workflow. If no such deterministic artifact can be demonstrated under the permanent compiler lanes, audit the next architectural frontier instead of manufacturing hand-written DWARF or synthetic memory ownership.
