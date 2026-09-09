# Phase 20 Roadmap — genuine-core stack-resident local recovery

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 20 extends immutable post-mortem source-value inspection to one ordinary compiler-owned stack local without turning the core reader into a generic DWARF expression engine or borrowing live ptrace state.

## Genuine compiler/core evidence

The permanent compiler lanes build the real `xmm_core_value_fixture` with a crash-frame `uint64_t stack_local = 0x4f3e2d1c0b9a8877` and generate an ordinary Linux `ET_CORE` artifact.

The branch first strengthened the DWARF oracle before production changed. At the exact crash probe:

- GCC emits a genuine `DW_OP_fbreg -32` location with `DW_AT_frame_base = DW_OP_call_frame_cfa`;
- Clang 18.1.3 in the permanent large-code-model lane emits a genuine `DW_OP_fbreg -8` location with `DW_AT_frame_base = DW_OP_reg7 (rsp)`;
- the pre-production gate kept all 54 normal CTest cases green in both lanes and failed only when immutable core inspection requested `stack_local`, because the snapshot evaluator intentionally accepted only the already-proven XMM0 / `DW_OP_addr` frame-zero ownership paths.

No DWARF bytes or frame-base expressions are hand-authored by the test.

## Completed executable slice

- snapshot local lookup accepts exactly one compiler-proven `DW_OP_fbreg` plus one checked SLEB128 offset; trailing expression operations remain rejected;
- the first stack-local path is deliberately bounded to selected frame zero and an already-supported 1–8 byte integer scalar;
- Clang's compiler-proven `DW_OP_reg7 (rsp)` frame base is taken from the selected immutable `SnapshotInspectionFrameContext` register state, never from a live task;
- GCC's compiler-proven `DW_OP_call_frame_cfa` frame base reuses the existing `.eh_frame` evaluator against an `EhFrameCursor` built from the selected immutable frame and a memory callback backed only by `read_snapshot_memory()`; the resulting caller cursor stack pointer is the evaluated CFA, matching the existing live frame-base semantics without ptrace;
- signed `DW_OP_fbreg` address arithmetic uses the existing checked `add_signed` helper and rejects underflow/overflow instead of wrapping;
- the final bytes are read only through `read_snapshot_memory()`, preserving authoritative core-memory ownership and the existing explicit runtime-artifact fallback/provenance rules;
- value construction reuses the existing snapshot memory materializer, preserving source name, integer width/signedness, module ownership, and `SnapshotCoreMemory` provenance;
- direct `CoreInspectionSession::inspect_value("stack_local")` recovers exactly `0x4f3e2d1c0b9a8877` from the genuine core;
- the real `mdbg-core` subprocess prints the same value in the existing crash-thread workflow alongside XMM, scalar-pointer dereference, aggregate-pointer dereference, and immutable thread selection;
- PIE and non-PIE artifacts are exercised under both permanent GCC and Clang-large CI lanes.

## Explicit fail-closed boundary

Phase 20 does not introduce a general DWARF stack machine. Snapshot `DW_OP_fbreg` remains restricted to the demonstrated single-operation form, frame zero, bounded integer storage, and exactly the two compiler-proven frame-base forms above. Compound frame-base expressions, other frame-base registers, caller-frame `DW_OP_fbreg`, stack-resident aggregates/pointers, nested expressions, locational arithmetic beyond the signed fbreg offset, and unsupported provenance remain explicit errors.

The evaluator does not substitute crash-frame registers for historical callers and does not fall back to live process state. Missing `.eh_frame`, unavailable immutable RSP, unsupported CFI, unreadable snapshot bytes, or ambiguous artifact fallback remain deterministic failures.

Phase 20 is therefore sealed after one coherent compiler/core-proven stack-local slice rather than expanding by enumerating offsets, scalar widths, structure variants, or additional DWARF opcodes.

## Phase 21 promotion — historical caller-frame stack-local ownership

The next architectural hypothesis is not another frame-zero location variant. It is whether a **recovered historical caller frame** can own a stack-resident source local without borrowing the crash frame's current registers.

A first Phase 21 slice must begin from a genuine GCC/Clang core artifact in which an already-recovered caller frame has a stable compiler-produced stack local at the selected caller PC. Production must not expand until an oracle proves the exact location and frame-base form.

The first accepted slice must prove:

1. `mdbg-core` selects a recovered caller frame through the existing immutable inspection-session ownership path;
2. the caller local is represented by genuine compiler DWARF, preferably the existing bounded `DW_OP_fbreg` family rather than a newly manufactured expression;
3. frame-base/CFA evaluation uses only historical state carried by that exact `SnapshotInspectionFrameContext`, existing CFI recovery, and snapshot-owned memory — never the crash frame's RSP/RBP or any live ptrace register;
4. the resulting address is read through `read_snapshot_memory()` with unchanged provenance/ambiguity rules and materialized through the existing bounded type model;
5. selecting another frame/thread or presenting a stale/mismatched frame cannot make old historical stack ownership valid again;
6. API and real `mdbg-core` subprocess coverage run under permanent GCC and Clang-large lanes, with PIE/non-PIE coverage where compiler output is deterministic;
7. if stable compiler evidence is not available, perform an architecture audit instead of hand-writing DWARF or growing a generic expression interpreter.

## Selection rule

Phase 21 starts only from a concrete compiler/core-produced caller-stack failure. Do not reopen Phase 20 for more frame-zero offsets, scalar widths, aggregate variants, or frame-base opcodes unless a separate real artifact demonstrates a distinct architectural need.
