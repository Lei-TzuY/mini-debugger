# Phase 57 Roadmap — compiler-proven frame-zero selected-inline fixed arrays

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 57 closes the frame-zero selected-inline fixed-array gap by reconstructing one compiler-owned small array from immutable crash registers and materializing it through the same canonical array model already used by caller-frame and historical physical values.

## Completed acceptance

- A dedicated optimized fixture retains one active fixed array inside the selected frame-zero inline scope at the exact crash probe.
- An independent DWARF oracle proves lexical ownership, exact bounded array type, element count/width/signedness, and the crash-PC-active compiler register-piece expression before product assertions run.
- Test-first head `cfeb12ec5776f1b9f492fe67f26f207361a68499` proves the compiler-owned array representation and fails before product support is added.
- Production head `187c9c9901399e7278f75c25296822253c344ca9` reuses the same bounded `evaluate_register_piece_value()` path already proven by Phase 56 structures. It does not add a parallel array expression evaluator.
- Frame-zero selected-inline structures and arrays share the same RDX/RCX-only register-piece reconstruction boundary. Unsupported register numbers, piece widths, trailing operations, and unsupported typed shapes remain explicit failures.
- The reconstructed array materializes through canonical `LocalValueType::array_type` and existing bounded array byte materialization, with `SnapshotCoreRegister` provenance for the array object.
- API and real `mdbg-core` evidence prove exact array rendering, checked element indexing, deterministic out-of-range rejection, and invalidation across physical-frame / inline-selection changes.
- The Phase 57 fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and the dedicated GCC / Clang compiler-evidence workflow.
- Exact production head `187c9c9901399e7278f75c25296822253c344ca9` passes full GCC / Clang-large CI plus both GCC / Clang dedicated evidence lanes.
- The candidate is based directly on main `d01d059e6ef102a802b2ebffeabf3e6bc399ea3b`, ahead by two commits and behind by zero.

Phase 57 is sealed here. More array lengths, alternate element widths, register permutations, multidimensional/VLA arrays, decay, slices, mutation, or arbitrary DWARF expression support are not new milestones.

## Phase 58 promotion — compiler-proven frame-zero selected-inline union ownership

The next executable gap is the explicit frame-zero selected-inline union guard. Caller-frame selected-inline and historical physical unions already have canonical bounded union typing, immutable materialization, explicit member selection, and no guessed active-member semantics; frame-zero union ownership is still rejected before the now-shared register-piece reconstruction can feed those same primitives.

The first coherent Phase 58 slice must:

1. Start from a genuine optimized GCC and Clang frame-zero inline artifact containing one small union that remains live at the crash PC.
2. Independently prove inline lexical ownership, exact union byte extent, direct bounded integer members sharing offset zero, and the crash-PC-active compiler location expression before product assertions run.
3. Accept only compiler-produced RDX/RCX register or register-piece forms actually emitted by both permanent compiler lanes. Do not generalize to arbitrary register numbers or expression forms without evidence.
4. Reuse the existing bounded register-piece byte reconstruction and canonical `resolve_bounded_union_type()`; do not create a union-specific expression evaluator or descriptor.
5. Materialize the union from reconstructed immutable bytes through one shared bounded union byte materializer. Preserve `SnapshotCoreRegister` provenance and explicit-member-only semantics; never infer an active member.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <union> -> union-member <union> <member>`, including exact overlapping interpretations, missing-member rejection, and physical/inline invalidation.
7. Keep nested unions, pointer/aggregate union members beyond existing proven scalar surface, discriminators/variant parts, mutation, arbitrary reinterpretation, and unsupported register forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion extends the frame-zero typed-object frontier while reusing the register-piece architecture established by Phases 56–57.
