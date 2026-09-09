# Phase 16 Roadmap — immutable extended thread machine state

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 16 extends immutable post-mortem thread state beyond the GPR-only `NT_PRSTATUS` surface. The implementation is deliberately evidence-driven: it supports only the extended-state note family that a genuine Linux core artifact produced under the repository's deterministic threaded crash workflow.

## Genuine kernel evidence

The repository's existing `formal_parameter_fixture --snapshot-crash-threaded` workflow was run under the normal Ubuntu 24.04 GitHub Actions core-dump configuration. The genuine two-thread core exposed this CORE-note sequence:

`NT_PRSTATUS(336) -> NT_PRPSINFO(136) -> NT_SIGINFO(128) -> NT_AUXV(368) -> NT_FILE(1196) -> NT_FPREGSET(512) -> NT_PRSTATUS(336) -> NT_FPREGSET(512)`

The same artifact contained two `NT_PRSTATUS` notes, two 512-byte `NT_FPREGSET` notes, and **no `NT_X86_XSTATE` note**. This proves that `NT_FPREGSET`, not XSTATE, is the first Phase 16 surface for the current runner/kernel environment.

The first FPREGSET is not adjacent to its PRSTATUS because process-global CORE notes appear between them. The bounded association rule is therefore: a CORE `NT_FPREGSET` belongs to the most recent `NT_PRSTATUS` thread context until another `NT_PRSTATUS` replaces that context. FPREGSET before any PRSTATUS is invalid, and one thread context may not own duplicate FPREGSET notes.

The threaded crash fixture seeds distinct XMM15 sentinels immediately before the crashing thread faults and before the sibling enters its syscall wait loop. An independent raw-note oracle recovered the exact values from the two genuine 512-byte descriptors, proving both the legacy x86-64 SSE layout and per-thread ownership rather than inferring either from ABI declarations alone.

## Completed executable slice

- `CoreFloatingPointState` exposes immutable `mxcsr`, `mxcsr_mask`, and XMM0–XMM15 values.
- `CoreSnapshot::floating_point_state(tid)` returns that state for a known core thread when a genuine 512-byte `NT_FPREGSET` is present.
- the parser accepts only little-endian x86-64 `ET_CORE` input and exact native `user_fpregs_struct` descriptors;
- FPREGSET association follows the genuine PRSTATUS-context ordering above rather than an adjacency assumption;
- orphan FPREGSET, duplicate FPREGSET ownership, malformed note bounds, and non-512-byte descriptors fail closed;
- absence of FPREGSET for a valid core thread remains an optional absence rather than inventing state;
- the permanent GCC and Clang-large CI lanes generate a genuine threaded core, validate its raw note/XMM evidence with an independent oracle, and separately validate `CoreSnapshot` recovery for the crash thread and sibling;
- the crash and sibling XMM15 values are intentionally distinct, so a thread-association regression cannot pass by collapsing both threads onto one state record.

## Explicit non-goals

- `NT_X86_XSTATE`, AVX/YMM/ZMM state, or XSAVE component enumeration without a genuine repository artifact that contains such a note;
- speculative x87 semantic presentation beyond preserving the proven bounded SSE-facing state;
- live ptrace vector-register mutation;
- generic CORE-note dumping or ABI-field enumeration that does not advance a concrete post-mortem workflow.

These are evidence gates, not missing-checkbox lists. Phase 16 should be reopened only when a genuine artifact demonstrates an independently useful unsupported extended-state family.

## Phase 17 promotion — SSE-backed DWARF value recovery

The next architectural frontier is **compiler-proven source-value recovery from immutable XMM state**, not more FPREGSET parsing.

Phase 17 begins only from a concrete failing compiler/debug-info scenario in which an optimized source value is described by DWARF as residing in an XMM register and `mdbg-core print` cannot currently recover it. The first coherent slice should:

1. produce a deterministic optimized fixture whose source value is genuinely assigned to an XMM DWARF register at the crash snapshot;
2. confirm the emitted DWARF register/location rule from the compiler artifact rather than assuming a register number or expression shape;
3. route the selected core thread's `CoreFloatingPointState` through the existing DWARF register/value provider without creating a parallel source-value evaluator;
4. make the existing post-mortem `print` workflow recover the concrete source value from the genuine core;
5. preserve thread selection: changing immutable core threads must change the XMM-backed register provider accordingly;
6. fail closed on unsupported vector widths, register numbers, expression shapes, or absent extended state.

Do not broaden Phase 17 into arbitrary SIMD inspection, AVX support, or DWARF opcode enumeration unless a genuine compiler-produced failing scenario requires that exact capability.

## Selection rule

Choose the highest-value compiler- and artifact-proven failing source-value scenario. If the current compilers do not emit an XMM-backed source value that the repository can demonstrate and validate, do not manufacture one with hand-written DWARF; audit the next architectural frontier instead.
