# Phase 5 roadmap — sealed

Phase 5 is complete for the current compiler-produced DWARF source-value milestone. The phase extended the live stopped-frame ownership model only when an active GCC or Clang current-PC workflow produced a concrete failure; it did not grow by enumerating DWARF opcodes.

The detailed Priority 0–8 contracts and evidence that led to this closure are preserved verbatim in `docs/phase-5-roadmap-history.md`.

## Completed priorities

- **P0 — entry-value ownership:** bounded `DW_OP_entry_value(DW_OP_reg5); DW_OP_stack_value` backed by debugger-observed function-entry register snapshots.
- **P1 — register-relative stack values:** compiler-proven `DW_OP_breg5 + signed offset + DW_OP_stack_value` with selected-TID register ownership.
- **P2 — RBX/XOR stack values:** bounded RBX-relative value composition with compiler constant/XOR semantics.
- **P3 — multi-register composition:** the GCC-selected R8/R9/RCX/RDX/RSI/RDI shift/XOR expression family through the bounded stack evaluator.
- **P4 — register-piece aggregates:** the compiler-produced 16-byte RDI/RSI `DW_OP_piece` structure ownership shape.
- **P5 — register-relative memory ownership:** bare compiler-selected `DW_OP_breg5` interpreted as an inferior-memory location rather than a stack value.
- **P6 — inlined lexical ownership:** concrete locals beneath `DW_TAG_inlined_subroutine` with bounded `DW_AT_abstract_origin` identity/type inheritance while concrete location remains authoritative.
- **P7 — const-qualified types:** bounded transparent `DW_TAG_const_type` ownership through supported `DW_AT_type`/`DW_FORM_ref4` links.
- **P8 — one-hop dereference stack values:** selected-TID RDI ownership, one 8-byte inferior-memory dereference, compiler constant/XOR, and final `DW_OP_stack_value`.
- **P9 — two-hop dereference stack values:** the same compiler-proven family extended only to one or two consecutive 8-byte dereferences.

## Priority 9: compiler double-dereference stack values — complete

The final bounded slice comes from a real pointer-to-pointer workflow rather than opcode enumeration. The existing `indirect_local` keeps the same observable `0x8877665544332211` value but receives a `uint64_t**` and computes `**ptr ^ 0x55aa00ff33cc6699`.

At `indirect_local_probe`, GCC 13.3 selects the active current-PC expression:

`DW_OP_breg5 (rdi): 0; DW_OP_deref; DW_OP_deref; DW_OP_constu 6172747335350380185; DW_OP_xor; DW_OP_stack_value`

Clang 18.1.3 materializes the same source value through the already-supported `DW_OP_reg0 (rax)` path.

Completed P9 contract:

- test-first run #721 leaves the complete Clang-large lane and the other 46 GCC tests green while GCC PIE/non-PIE fail only when the previous evaluator reaches the second `DW_OP_deref`;
- the evaluator admits exactly one or two consecutive dereferences in this expression family; each dereference is one bounded x86-64 8-byte inferior-memory read through existing `Debugger::read_memory` ownership on the selected stopped TID;
- the same direct API and real CLI workflow proves the unchanged expected value for PIE and non-PIE; production run #728 and later normal branch CI remain fully green under GCC and Clang-large;
- nonzero breg offsets, alternate dereference widths, chains deeper than two, malformed suffixes, arbitrary stack operations, and trailing data remain fail-closed.

P9 does not create an arbitrary pointer-chasing VM. It extends the compiler-proven ownership chain from one inferior-memory load to two and leaves every other boundary explicit.

## Phase 5 closure

Further expression work must not continue as Priority 10+ opcode farming. A new expression form may be reopened only when a higher-level executable workflow selects a concrete compiler-generated failure that cannot be represented safely by the existing ownership model.

The active architectural frontier is now **frame-aware inspection**. Source values outside the current live frame require explicit frame PC/module identity, unwind-recovered register state, process/TID-owned memory, type, and lexical-scope ownership instead of implicit reads from the active callee register file.

## Selection rule

Phase 5 is sealed. Preserve the fail-closed evaluator and permanent compiler matrix. Continue in `docs/phase-6-roadmap.md`.
