# Phase 48 Roadmap — compiler-proven physical nested aggregate composition

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 48 closes the remaining one-hop nested-aggregate asymmetry between selected-inline and historical physical-frame inspection. A historical physical caller can now own, materialize, and explicitly traverse exactly one by-value inner structure through the same bounded type graph and byte-owned traversal primitives used by selected-inline inspection.

## Completed acceptance

- The permanent historical caller fixture now retains a real stack-resident `caller_nested_aggregate` shaped as an eight-byte outer structure with signed `prefix@0` and a four-byte by-value `inner@4`; the inner structure owns signed `terminal@0`.
- An independent readelf oracle proves GCC and Clang both retain the variable through compiler-produced historical `DW_OP_fbreg` ownership, the exact outer/inner sizes, constant offsets, signed 32-bit terminal types, caller lookup PC, and existing frame-base forms before product assertions execute.
- Test-first evidence reached a genuine core under both compiler lanes and failed at the intended architecture boundary: the ordinary physical structure resolver attempted to force the structure-valued `inner` member through the scalar base-type chain.
- One canonical `resolve_bounded_nested_structure_member_type()` now serves ordinary physical and selected-inline structure roots. The old 160+ line selected-inline-only nested type resolver is removed.
- One canonical `materialize_bounded_nested_structure_member()` decodes the inner structure strictly from bytes already owned by the outer aggregate. Live structure decoding, immutable snapshot decoding, and selected-inline materialization share that bounded primitive.
- Compiler-owned relative offsets are retained for both the outer `inner` member and inner `terminal`; runtime-artifact provenance therefore composes through the existing member-selection storage rules without a second memory search.
- `CoreInspectionSession::inspect_nested_aggregate_member()` now resolves physical or selected-inline source ownership first and then invokes one context-neutral bounded nested traversal primitive.
- Real API evidence proves `frame 1 -> inline physical -> print caller_nested_aggregate -> nested-aggregate-member caller_nested_aggregate inner terminal`, exact terminal value recovery, immutable core provenance, and invalidation across thread/frame changes.
- Real `mdbg-core` exercises the same physical nested traversal command.
- The exact production head passes full GCC / Clang-large CI and the dedicated GCC / Clang selected-inline regression/evidence workflow.

Phase 48 is sealed here. Additional terminal widths, alternate offsets, more direct members, or another nested depth are not new milestones. A structure-valued terminal, arbitrary dotted paths, recursive graphs, cycles, inheritance, dynamic member locations, flexible arrays, mutation, and implicit dereference remain rejected.

## Phase 49 promotion — compiler-proven physical fixed-array ownership and indexing

The next meaningful context gap is fixed arrays. Selected-inline inspection already owns bounded compiler-proven arrays and explicit indexing, while a historical physical frame cannot yet describe or materialize an array root through the ordinary snapshot type path.

The first coherent Phase 49 slice must:

1. Start from a genuine GCC and Clang historical caller core where one stack-resident fixed-size array has a compiler-produced location supported by the existing immutable frame-base machinery. Do not synthesize array DWARF.
2. Independently prove the array DIE, bounded element count, element byte width/signedness, total byte extent, selected lookup PC, and location/frame-base form before product assertions run.
3. Extend the canonical root type description only enough to carry an already-bounded fixed-array element descriptor. Reuse the selected-inline array limits and reject dynamic bounds, variable-length arrays, multidimensional recursion, and unsupported element kinds.
4. Materialize the array from bytes already owned by the physical root through `read_snapshot_memory()`, preserving immutable core/runtime-artifact provenance and exact element byte offsets.
5. Make `CoreInspectionSession::inspect_array_element()` context-neutral: physical and selected-inline roots resolve source ownership separately, then share one bounded indexing/provenance primitive.
6. Prove real `mdbg-core` behavior for `frame 1 -> inline physical -> print <array> -> array-element <array> <index>`, plus deterministic out-of-range rejection and thread/frame invalidation.
7. Keep the first slice one-dimensional and scalar-element only. No pointer arithmetic, arbitrary expressions, slices, multidimensional recursion, mutation, or implicit decay-to-pointer behavior.
8. Require full GCC / Clang-large CI plus the relevant permanent compiler/oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion continues outward with executable historical-frame capability rather than returning to low-value descriptor churn.
