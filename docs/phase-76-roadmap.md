# Phase 76 Roadmap — compiler-proven current-frame live fixed-array ownership and checked indexing

Status: complete for the current Linux x86-64 optimized live-debugger milestone.

Phase 76 closes the fixed-array ownership gap for the current stopped live frame. A genuine optimized local fixed array can now be recovered from compiler-produced stack ownership, materialized through the canonical bounded array representation, rendered by the live CLI, and indexed through the existing checked context-neutral array selector.

## Completed acceptance

- The optimized permanent fixture retains a genuine current-frame `int32_t live_array[3]` with exact values `0x10203040`, `0x22334455`, and `0x33445566` at `live_array_probe`.
- The independent oracle proves the real `DW_TAG_array_type`, exactly one bounded zero-based subrange with three elements, signed four-byte integer element type, twelve-byte total extent, lexical ownership, exact probe coverage, and compiler-produced `DW_OP_fbreg` location before product assertions run.
- Test-first head `5db3f865dc56a89bbe6256ee236aa947d769f07a` keeps both dedicated compiler-evidence lanes green and all pre-existing tests green, then fails only in the new PIE/non-PIE live-array workflow at the explicit product guard `live fixed-array materialization is outside current compiler evidence`.
- Production head `91d42a2605f66a26e45fdc64be70d5a5ef071e4a` reuses the canonical array model instead of introducing a live-only descriptor/parser/indexer:
  - `materialize_bounded_array_bytes()` now has an ownership-neutral module-path overload while the immutable snapshot wrapper remains intact;
  - live `DW_OP_fbreg` array ownership reads exactly the compiler-described byte extent from the stopped tracee and feeds those bytes to the shared array materializer;
  - non-fbreg array forms remain rejected rather than falling through scalar evaluators.
- Compiler evidence exposed two implementation plumbing gaps during convergence:
  - `91d42a26` initially revealed an include-order declaration issue; `56cc21c9` adds only the shared materializer forward declaration;
  - Clang then proved its real `DW_AT_frame_base` form is `DW_OP_reg7 (rsp)`; final head `92139113` accepts exactly this current-frame RSP form alongside the previously-supported RBP/CFA forms. No arbitrary register frame-base evaluator was introduced.
- API evidence proves canonical `LocalValueKind::Array` identity, exact element metadata and values, checked index 1 recovery, and deterministic index-3 rejection.
- The live `mdbg` CLI now renders the bounded array and exposes `array-element <name> <index>` through the same `inspect_local_array_element()` primitive. It proves exact index recovery and explicit out-of-range failure.
- Existing scalar, enum, pointer, live structure, historical live traversal, post-mortem array, selected-inline array, pointer dereference, CFI, and provenance suites remain green.
- Exact production head `92139113e853c0f35aeac6d4eddd57ecf0964813` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler/core/session/CLI evidence lanes, based directly on main `c2d55a8fa5e18046e590e330acc3f927844dc09a`, ahead four / behind zero.

Phase 76 is sealed here. Additional array lengths, widths, fbreg offsets, compiler spellings, multidimensional/VLA/flexible arrays, decay, slices, mutation, or arbitrary location expressions are not new milestones.

## Phase 77 promotion — compiler-proven current-frame live union ownership and explicit member selection

The next executable frontier is the remaining canonical aggregate root still explicitly rejected by current live inspection. Bounded union typing, overlapping-storage byte materialization, explicit member selection, rendering semantics, and compiler evidence already exist across physical core and selected-inline ownership, while current live ptrace lookup still fails at `live union materialization is outside current compiler evidence`.

The first coherent Phase 77 slice must:

1. Start from a genuine optimized GCC and Clang current-frame fixture with one small bounded union local active at an exact debugger stop.
2. Independently prove the real `DW_TAG_union_type`, exact byte extent, direct overlapping member names/types/offset-zero storage, lexical ownership, exact stop PC, and compiler-produced active location form before product assertions run.
3. Accept only location/frame-base forms actually emitted by both permanent compiler lanes. Reuse existing live frame-base/address or bounded register-piece machinery as appropriate; do not generalize arbitrary DWARF expressions or register layouts.
4. Reuse canonical `resolve_bounded_union_type()`, `materialize_bounded_union_bytes()`, and `inspect_local_union_member()`. No live-only union descriptor, parser, decoder, or selector is allowed.
5. Expose explicit live API member views without inferring an active union member. Both overlapping signed/unsigned views must decode the same owned bytes using their own compiler-described metadata.
6. Extend the real live `mdbg` CLI coherently so `print <union>` plus one explicit `union-member <union> <member>` workflow is executable, with deterministic missing-member rejection.
7. Preserve stopped-tracee freshness and reject unsupported/non-current ownership after execution advances.
8. Keep discriminators/variant parts, active-member inference, nested unions, pointer-member expansion, reinterpret casts, mutation, arbitrary expressions, and unsupported location forms out of scope.
9. Require full GCC / Clang-large CI plus permanent compiler-oracle/API/CLI evidence on the exact candidate before integration.

This promotion adds a new live typed-object capability rather than farming fixed-array variants.
