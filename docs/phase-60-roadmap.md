# Phase 60 Roadmap — compiler-proven frame-zero selected-inline nested aggregate composition

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 60 composes the canonical one-hop nested-structure model with genuine frame-zero selected-inline ownership. The permanent GCC and Clang lanes both retain the active nested aggregate through an exact compiler-produced `DW_OP_fbreg` stack location, so the implementation follows physical frame-zero frame-base ownership instead of forcing register pieces.

## Completed acceptance

- A dedicated optimized fixture retains one active eight-byte `inline_nested` outer structure in the selected frame-zero inline scope. The outer value contains signed 32-bit `prefix = 0x10203040` at offset 0 and one by-value four-byte inner structure at offset 4 whose signed 32-bit `terminal = 0x55667788` is at offset 0.
- An independent readelf oracle proves the inline lexical chain, exact outer/inner DIEs, byte extents, direct member offsets, signed terminal/base types, crash-probe PC, and the exact active compiler location before product assertions run.
- Test-first head `dc73ac8c95763760039690399f396097db7c2ba9` establishes the genuine compiler-owned shape and fails before the frame-zero nested composition path exists.
- Production head `f0ea71b3f16fe081d9761a84879197cdc8a2e40c` adds only one bounded frame-zero nested-structure shape gate to the existing selected-inline `DW_OP_fbreg` path.
- The gate accepts exactly one direct scalar plus one by-value nested structure containing exactly one terminal integer member. It does not introduce deeper nesting, arrays/unions within the nested member, recursive traversal, or a new object graph.
- Type recovery reuses canonical `resolve_bounded_nested_structure_member_type()` and existing `LocalStructMemberType` nested metadata.
- Runtime ownership reuses the physical frame-zero snapshot frame base and immutable snapshot-memory reader; materialization reuses the existing generic snapshot structure/nested byte path.
- Explicit traversal reuses the context-neutral `inspect_nested_aggregate_member()` surface; selecting an inline context does not create an inline register file, stack, CFA, or memory image.
- API evidence recovers the exact outer, inner, and terminal values; preserves `SnapshotCoreMemory` provenance; and invalidates ownership after returning to the physical frame.
- Real `mdbg-core` executes `inline <active> -> print inline_nested -> nested-aggregate-member inline_nested inner terminal -> inline physical`.
- The fixture, compiler oracle, core/session/CLI integration, and workflow wiring are permanent in both normal GCC / Clang-large CI and the dedicated GCC / Clang compiler-evidence lanes.
- Exact production head `f0ea71b3f16fe081d9761a84879197cdc8a2e40c` passes full GCC / Clang-large CI plus both dedicated evidence lanes and is based directly on main `84245c3df101eaaaa0b0b418444081b3c1557581`, ahead two / behind zero.

Phase 60 is sealed here. Deeper nesting, alternate offsets, another terminal width, arrays/unions inside the nested member, recursive graphs, mutation, arbitrary dotted expressions, or register-forcing fixture tricks are not new milestones.

## Phase 61 promotion — frame-zero selected-inline typed aggregate pointer-member traversal

The next meaningful executable composition gap is a frame-zero selected-inline structure containing a bounded pointer-valued direct member. Phase 45 already proves context-neutral typed aggregate member selection and one-hop pointer-member dereference for immutable historical/caller-frame aggregates. Phase 56 proves frame-zero selected-inline direct-structure ownership. Those capabilities have not yet been composed at frame zero.

The first coherent Phase 61 slice must:

1. Start from genuine optimized GCC and Clang frame-zero inline artifacts containing one bounded structure with a compiler-described pointer-valued direct member and a stable integer pointee. Do not force a register/stack representation merely to match an existing evaluator.
2. Independently prove lexical ownership, exact structure byte extent, pointer-member name/type/offset, x86-64 pointer width, bounded integer pointee metadata, crash-PC-active location expression, and pointee value/address before product assertions run.
3. Follow the compiler-produced physical ownership form actually emitted by both permanent lanes. Reuse exact proven register/register-piece or bounded `DW_OP_fbreg` machinery; extend only the minimal evidence-required ownership form if neither existing path applies.
4. Reuse canonical `resolve_bounded_direct_structure_member_type()`, pointer member metadata, immutable structure materialization, context-neutral aggregate-member selection, and existing one-hop aggregate pointer-member dereference. No frame-zero-specific pointer descriptor or second dereference engine is allowed.
5. Preserve distinct provenance for the aggregate/pointer object and the pointee read. Inline selection must not invent a register file, CFA, stack, or memory image.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <aggregate> -> aggregate-member <aggregate> <pointer-member> -> deref-aggregate-member <aggregate> <pointer-member>`, including exact pointee recovery and physical/inline invalidation.
7. Keep pointer chains, pointer arithmetic, implicit dereference, null fabrication, recursive graphs, mutation, arbitrary expressions, and unsupported ownership forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion composes two already-proven typed-value capabilities into a new frame-zero executable path rather than farming more nested-aggregate variants.
