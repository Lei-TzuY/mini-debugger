# Phase 56 Roadmap — compiler-proven frame-zero selected-inline direct structures

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 56 closes the frame-zero selected-inline direct-structure gap by composing one small compiler-owned structure from immutable crash registers without inventing inline machine state or a general DWARF expression VM.

## Completed acceptance

- A dedicated optimized fixture retains one active `FrameZeroInlinePair inline_pair` inside the selected `frame_zero_inline_structure_inner` scope at the exact crash probe.
- The independent oracle proves the real inline call chain, exact eight-byte structure extent, two signed 32-bit members at offsets 0 and 4, and the crash-PC-active compiler location before product assertions run.
- GCC and Clang emit only the two independently proven piece orderings:
  - `DW_OP_reg1 (rdx); DW_OP_piece: 4; DW_OP_reg2 (rcx); DW_OP_piece: 4`
  - `DW_OP_reg2 (rcx); DW_OP_piece: 4; DW_OP_reg1 (rdx); DW_OP_piece: 4`
- Test-first head `bab5822952e90a88618718f4e717678ec52f3c48` exposed the unsupported compiler representation. Follow-up parser head `cf1d3aae810ac9cc70108a88bdf70a0dad27e5bf` added the genuine `DW_FORM_sdata` support required by compiler-produced attributes rather than weakening the oracle.
- Production head `655fbee213b5e5e0d02f8d629b963b2856118020` extends the existing bounded register-piece evaluator with exactly the eight-byte / two-int32 / RDX+RCX piece shape. The pre-existing 16-byte RDI/RSI signal-restored structure shape remains supported by the same helper.
- Unsupported register numbers, piece widths, member shapes, trailing operations, and non-structure values remain explicit failures.
- Frame-zero selected-inline structure materialization reuses the canonical `LocalValueType`, canonical direct-member typing, the physical crash thread's immutable register snapshot, and the existing structure decoder. No inline register file, CFA, stack frame, or alternate object representation is introduced.
- API evidence proves `inline_pair` materializes as an eight-byte structure with exact `first = 0x11223314` and `second = 0x55667714`, `SnapshotCoreRegister` provenance, direct-member selection, and physical-frame invalidation.
- Real `mdbg-core` proves `inline <inner> -> print inline_pair -> aggregate-member inline_pair second -> inline physical`.
- The Phase 56 fixture/oracle/integration is permanently wired into both normal CI and the dedicated GCC/Clang compiler-evidence workflow; these workflow changes are regression coverage, not construction-only triggers.
- Exact production head `655fbee213b5e5e0d02f8d629b963b2856118020` passes full GCC / Clang-large CI plus both GCC / Clang dedicated compiler/core/session/CLI evidence lanes.
- The candidate is based directly on main `6e646fa4e1ffb448b966455acb7835bcc434780a`, ahead by three commits and behind by zero.

Phase 56 is sealed here. More structure sizes, reordered members, extra registers, speculative piece widths, nested structures, or arbitrary expression support are not new milestones.

## Phase 57 promotion — compiler-proven frame-zero selected-inline fixed-array ownership

The next executable gap is the explicit frame-zero fixed-array guard. Caller-frame selected-inline fixed arrays already have canonical type metadata, immutable materialization, checked indexing, API behavior, and CLI rendering, while frame-zero selected-inline arrays are still rejected before compiler-owned register pieces can be converted into the same bounded array representation.

The first coherent Phase 57 slice must:

1. Start from a genuine optimized GCC and Clang frame-zero inline artifact containing one small fixed array with deterministic integer elements that remains live at the crash PC.
2. Independently prove lexical ownership, exact array byte extent, element count/width/signedness, and the crash-PC-active compiler location expression before product assertions run.
3. Accept only compiler-produced register/register-piece forms actually emitted by both permanent lanes. Do not generalize to arbitrary register numbers or arbitrary piece layouts without evidence.
4. Reuse or minimally generalize the existing bounded register-piece byte reconstruction so structures and arrays do not grow parallel expression evaluators.
5. Materialize through the canonical `LocalValueType::array_type` and existing immutable array materializer, preserving `SnapshotCoreRegister` ownership for the array object.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <array> -> array-element <array> <index>`, including exact element values, deterministic out-of-range rejection, and frame/thread/inline invalidation.
7. Keep multidimensional/VLA arrays, decay, slices, mutation, recursive object graphs, arbitrary expressions, unions, and unsupported register forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion adds another frame-zero typed-object capability while reusing the register-piece architecture proven by Phase 56.
