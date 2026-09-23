# Phase 62 Roadmap — compiler-proven frame-zero selected-inline enum-valued aggregate composition

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 62 composes canonical enum identity with genuine frame-zero selected-inline aggregate ownership. The permanent GCC and Clang lanes both retain one active eight-byte structure through exact compiler-produced `DW_OP_fbreg` stack ownership, and the product now preserves the enum-valued member's symbolic + numeric identity through both aggregate rendering and explicit member selection.

## Completed acceptance

- A dedicated optimized fixture retains one active `FrameZeroInlineEnumAggregate inline_enum_aggregate` in the selected frame-zero inline scope.
- The structure is exactly eight bytes: signed 32-bit `direct = 0x31415926` at offset 0 and `FrameZeroInlineMode mode = FrameZeroInlineBusy` at offset 4.
- The compiler-owned enum is exactly four bytes, unsigned, and carries the finite table `FrameZeroInlineIdle = 3`, `FrameZeroInlineReady = 7`, `FrameZeroInlineBusy = 42`.
- An independent readelf oracle proves lexical ownership, structure/member DIEs, exact byte extents/offsets, enum representation, enumerator table, crash probe, and active compiler location before product assertions run.
- GCC emits direct `DW_OP_fbreg -32` for PIE and non-PIE artifacts. Clang emits direct `DW_OP_fbreg -8` for PIE and non-PIE artifacts. No register forcing, synthetic DWARF, or guessed ownership form is used.
- Exact red-test head `d4668a7141948d96dd97ef1e6f68a2bbb5c5b4a2` proves the genuine compiler-owned shape and fails only because the frame-zero direct-structure policy had no enum-valued shape.
- Production head `ffee7d5f9887a2ff71407e36c9d8f259e3de9724` adds one bounded frame-zero enum-structure eligibility gate to the already-existing `DW_OP_fbreg` immutable snapshot path. It introduces no enum parser, materializer, symbolic table, frame base, or memory reader.
- Canonical `resolve_bounded_enum_type()`, canonical direct-structure member typing, generic immutable structure materialization, context-neutral `inspect_aggregate_member()`, and conservative `local_enum_symbol()` remain the only type/value paths.
- API evidence proves exact `direct` and `mode` values, bounded enum metadata, symbolic lookup to `FrameZeroInlineBusy`, unknown-value numeric fallback, duplicate-alias ambiguity, aggregate/member provenance, and physical/inline invalidation.
- The first production pass exposed a separate generic CLI gap: structure containers rendered every direct member as raw hex even when the member already carried canonical enum metadata.
- Head `3a7d2208268f46c4303fd58b96754191c01e027f` generalizes the existing conservative enum-symbol helper to `LocalStructMember` and makes the ordinary structure renderer preserve symbolic + numeric identity for enum-valued members. No frame-zero-specific renderer is introduced.
- Existing historical physical enum-aggregate regression coverage is strengthened to require the same symbolic + numeric container rendering rather than retaining the old lossy `mode=0x2a` expectation.
- Exact candidate `a6e791e9bba24953e4d9afe045f6d41c3a233afc` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler-oracle/core/session/CLI evidence lanes.
- The candidate is based directly on main `88cb92b52a42ffd1ca6710b92b470f1d03c7379e`, ahead four / behind zero before the phase seal, with no unsupported ownership widening.

Phase 62 is sealed here. Enum arrays/unions, alternate enumerator tables, extra scalar members, signed-enum variants, aliases manufactured only for coverage, flags decomposition, enum arithmetic, mutation, recursive graphs, and arbitrary expressions are not new milestones.

## Phase 63 promotion — canonical frame-zero fbreg structure eligibility

Phases 59–62 added four separately named frame-zero structure shape gates for compiler-proven `DW_OP_fbreg` ownership: bit-field structures, one-hop nested structures, pointer-valued structures, and enum-valued structures. All of them feed the same physical frame-zero frame base, immutable snapshot-memory reader, canonical type metadata, and generic materializer. The growing inline boolean policy is now the architectural duplication.

The next coherent Phase 63 slice must:

1. Replace the four ad hoc `frame_zero_*_structure` booleans with one ownership-neutral, bounded frame-zero `DW_OP_fbreg` structure eligibility policy.
2. Preserve exactly the already-proven accepted shapes; do not broaden to arbitrary mixtures merely because metadata can be represented.
3. Keep shape validation explicit and reviewable: bit-field-only, one-direct-scalar + one one-hop nested structure, one pointer-to-signed-int32 member, and one signed-int32 + one bounded unsigned enum member.
4. Keep ownership evaluation separate from type-shape classification. The helper may decide whether a `LocalValueType` is eligible for the proven frame-zero fbreg path, but it must not read registers/memory, decode DWARF locations, or materialize values.
5. Delete the duplicated boolean bookkeeping and centralize diagnostics/invariants without wrapping the old code and leaving both policies alive.
6. Preserve the permanent Phase 59–62 GCC/Clang compiler-oracle/core/session/CLI evidence byte-for-byte in behavior, including provenance and invalidation.
7. Add focused unit/equivalence coverage only if needed to prove the classifier boundary; do not manufacture new compiler fixtures solely to exercise a refactor.
8. Require full GCC / Clang-large CI plus dedicated compiler evidence on the exact candidate before integration.

Phase 63 is an architecture-depth promotion after eight consecutive executable frame-zero expansions. Its purpose is to prevent the next genuine compiler-backed capability from extending a growing shape-switch thicket.
