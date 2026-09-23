# Phase 64 Roadmap — compiler-proven frame-zero selected-inline fixed-array stack ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 64 closes the remaining frame-zero selected-inline fixed-array ownership gap for stack-resident arrays. Phase 57 already proved bounded fixed arrays reconstructed from RDX/RCX register pieces; this phase proves the same canonical array model when a genuine optimized compiler keeps the active inline array in frame-zero stack storage through `DW_OP_fbreg`.

## Completed acceptance

- A dedicated optimized fixture retains one active two-element `int32_t inline_fbreg_array[2]` in the selected frame-zero inline scope at the exact crash probe.
- An independent readelf oracle proves the inline lexical chain, exact `DW_TAG_array_type`, one bounded direct subrange, element count 2, signed 32-bit element type, total byte extent 8, crash PC, active location range, and one exact compiler-produced `DW_OP_fbreg` expression before product assertions run.
- GCC and Clang permanent lanes independently satisfy that genuine stack-ownership contract. No register-piece form is forced for this fixture.
- Test-first head `0fcde33aae4b697564a852ff0055eb227f96183f` keeps the established typed-value suite intact, reaches the genuine compiler/core artifact, and then fails at the intended dispatcher seam: the frame-zero array is incorrectly routed into the register-piece evaluator and rejected with `8-byte register-piece local requires compiler-proven RDX/RCX pieces`.
- Production head `3b4e1e49019df7f7cbc68df400fca6733e62e8a3` extends the ownership-neutral frame-zero fbreg eligibility boundary only for the independently proven bounded array shape: exactly two signed four-byte integer elements with canonical array metadata and eight-byte total extent.
- The selected-inline value path then reuses the existing physical frame-zero frame-base computation, checked `DW_OP_fbreg` offset evaluation, immutable snapshot-memory reader, generic `materialize_snapshot_memory_value()` array path, and context-neutral checked array indexing.
- No frame-zero-specific array descriptor, parser, materializer, indexer, memory reader, or synthetic inline CFA/stack is introduced.
- Register-piece fixed-array behavior from Phase 57 remains on its existing path. Unions, arbitrary structures, multidimensional arrays, dynamic bounds, flexible arrays, unsupported register expressions, and unsupported location forms are not silently admitted.
- API evidence proves exact values `[0x10203014, 0x40506014]`, index 1 recovery, deterministic index-2 rejection, `SnapshotCoreMemory` provenance, and invalidation when returning to the physical frame.
- Real `mdbg-core` exercises `inline <active> -> print inline_fbreg_array -> array-element inline_fbreg_array 1` plus out-of-range rejection.
- Exact production head `3b4e1e49019df7f7cbc68df400fca6733e62e8a3` passes full GCC / Clang-large CI plus both GCC / Clang dedicated compiler/core/session/CLI evidence lanes and is based directly on main `36dc0275d388841a80846e897b91f81d23fd1d82`, ahead two / behind zero before the phase seal.

Phase 64 is sealed here. Additional array lengths, element widths, fbreg offsets, or compiler spellings are not new milestones.

## Phase 65 promotion — compiler-proven frame-zero selected-inline union stack ownership

The next executable ownership gap is the same machine-state distinction for unions, not another union-member variant. Phase 58 already proves bounded frame-zero selected-inline union semantics when the compiler owns the bytes in crash registers; canonical union type recovery, immutable byte materialization, explicit member selection, and provenance are mature. What remains unproven is a genuine active frame-zero inline union whose object bytes are stack-owned through `DW_OP_fbreg`.

The first coherent Phase 65 slice must:

1. Start from a genuine optimized GCC and Clang crash-frame artifact where one active selected-inline bounded union is retained through compiler-produced frame-zero `DW_OP_fbreg`. Do not hand-author the expression or force the compiler away from its natural representation.
2. Independently prove inline lexical ownership, exact `DW_TAG_union_type`, byte extent, overlapping direct member names/types/offset-zero semantics, crash PC, active location range, and exact compiler-produced `DW_OP_fbreg` ownership before product assertions run.
3. Reuse the selected physical frame's existing frame-base computation and immutable snapshot-memory reader. Inline selection remains lexical/type ownership only and must not invent an inline CFA, stack, or memory image.
4. Reuse canonical `resolve_bounded_union_type()`, generic immutable union byte materialization, context-neutral explicit union-member selection, and exact provenance. No frame-zero-specific union parser/materializer/selector is allowed.
5. Extend the frame-zero fbreg eligibility boundary only as far as the independently proven bounded union shape requires. Do not infer an active union member and do not admit arbitrary aggregate shapes by category alone.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <union> -> union-member <union> <member>`, including both overlapping interpretations, deterministic missing-member rejection, and invalidation after inline/frame/thread changes.
7. Keep active-member inference, discriminators/variant parts, nested unions, arbitrary reinterpretation, mutation, recursive graphs, and unsupported compiler location forms out of scope.
8. Require full GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion adds a new compiler-proven machine-state ownership form to an already canonical value model instead of farming union field variants.
