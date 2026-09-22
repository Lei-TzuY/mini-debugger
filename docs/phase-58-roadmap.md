# Phase 58 Roadmap — compiler-proven frame-zero selected-inline union ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 58 closes the remaining frame-zero selected-inline union gap by reconstructing one compiler-owned small union from immutable crash registers and materializing it through the same canonical bounded union model already used by caller-frame and historical physical values.

## Completed acceptance

- A dedicated optimized fixture retains one active four-byte union inside the selected frame-zero inline scope at the exact crash probe.
- An independent DWARF oracle proves the inline lexical chain, exact four-byte `DW_TAG_union_type`, the two compiler-described `signed_value:int32` / `unsigned_value:uint32` members sharing offset zero, and the exact crash-PC-active location expression before product assertions run.
- The evidence converges on two genuine permanent-lane compiler forms: `DW_OP_reg1 (rdx); DW_OP_piece: 4` and `DW_OP_reg2 (rcx)`. Unsupported register numbers, piece widths, trailing operations, and broader expression forms remain rejected.
- Test-first heads `60b17aaba890a1b8f12396d10bf2362a6437341a` and `f0648c6a35de07d88b32f50f2ac7a878b6471a0c` expose and refine the genuine compiler ownership surface before product support is accepted.
- Production head `ff0c721c9a7d4e425ded77575e9994b6697754d6` reuses the existing bounded RDX/RCX register-piece reconstruction already established by Phases 56–57. No union-specific register evaluator or second union descriptor is introduced.
- The selected-inline value path resolves the union through canonical `resolve_bounded_union_type()`, reconstructs the immutable owned bytes, and materializes them through shared `materialize_bounded_union_bytes()`.
- The union object preserves `SnapshotCoreRegister` provenance. Explicit member views reuse the existing context-neutral union-member selector and preserve the same register provenance.
- API and real `mdbg-core` evidence prove exact overlapping signed/unsigned member interpretations, missing-member rejection, and invalidation when returning to the physical frame. No active member is guessed.
- The Phase 58 fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and the dedicated GCC / Clang compiler-evidence workflow.
- Exact production head `ff0c721c9a7d4e425ded77575e9994b6697754d6` passes full GCC / Clang-large CI plus both GCC / Clang dedicated evidence lanes and is based directly on main `f578a9014a3cc07e9904e2293a21ddfb2c7e69a6`, ahead three / behind zero.

Phase 58 is sealed here. More union members, alternate scalar widths, register permutations, guessed active-member semantics, nested unions, mutation, discriminators/variant parts, or arbitrary reinterpretation are not new milestones.

## Phase 59 promotion — compiler-proven frame-zero selected-inline bit-field structures

The next meaningful executable integration gap is sub-byte aggregate semantics at frame zero. Canonical bit-field type recovery and immutable bit extraction already exist from Phase 51, while Phases 56–58 now prove frame-zero register-piece reconstruction for structures, arrays, and unions. Those two capabilities are not yet composed into one frame-zero selected-inline bit-field structure.

The first coherent Phase 59 slice must:

1. Start from a genuine optimized GCC and Clang frame-zero inline artifact containing one small structure with retained signed and unsigned bit fields that remains live at the crash PC.
2. Independently prove inline lexical ownership, exact aggregate byte extent, compiler-described bit widths and bit-location spelling, and the crash-PC-active register/register-piece expression before product assertions run.
3. Accept only the RDX/RCX register or register-piece forms actually emitted by the permanent compiler lanes. Do not generalize register coverage merely to make the fixture pass.
4. Reuse the canonical `resolve_bounded_bit_field_member_type()` / `LocalBitSlice` metadata and the existing bounded register-piece byte reconstruction. Do not introduce a frame-zero-only bit parser or decoder.
5. Materialize reconstructed bytes through the same immutable bounded structure byte path used by snapshot-backed structures, so signed normalization and bounds checks come from one shared bit-field decoder rather than the live-process structure decoder.
6. Prove exact signed and unsigned bit-field values, aggregate-member selection, `SnapshotCoreRegister` provenance, CLI rendering, and invalidation after physical-frame / inline-selection changes.
7. Keep arbitrary bit slicing, writes, endian/ABI expansion, nested bit-field aggregates, recursive graphs, and unsupported expression forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion composes two already-proven subsystems into a new executable frame-zero capability rather than farming another register or union variant.
