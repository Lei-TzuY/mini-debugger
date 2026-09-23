# Phase 61 Roadmap — compiler-proven frame-zero selected-inline typed aggregate pointer-member traversal

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 61 composes two previously independent capabilities: frame-zero selected-inline by-value structure ownership and context-neutral pointer-valued aggregate member traversal. The resulting path can inspect a pointer member owned by a selected frame-zero inline structure and dereference exactly one bounded signed-int32 pointee without inventing inline machine state or a second traversal engine.

## Completed acceptance

- A dedicated optimized fixture retains one active eight-byte `FrameZeroInlineTyped inline_typed` in the selected `frame_zero_inline_typed_inner` scope. The structure contains exactly one direct member, `linked@0`, an x86-64 pointer to signed 32-bit integer storage.
- The pointer targets the genuine static payload `frame_zero_inline_typed_payload = 0x02468ace`; product assertions recover that value through the immutable core/runtime-artifact memory reader rather than copying the source initializer into debugger state.
- An independent readelf oracle proves the inline lexical chain, exact eight-byte structure extent, pointer-member name/type/offset, pointer width, signed-int32 pointee metadata, exact crash probe, and active compiler-produced ownership form before product assertions run.
- The permanent GCC lane emits direct `DW_OP_fbreg -32` ownership for PIE and non-PIE artifacts. The permanent Clang lane emits direct `DW_OP_fbreg -8` ownership for PIE and non-PIE artifacts. No register forcing, synthetic DWARF, or guessed location form is used.
- Test-first work progressed through genuine compiler retention issues until the addressable inline aggregate shape was stable. The final production change is deliberately small: it recognizes exactly one frame-zero structure member that is a pointer at offset zero with x86-64 width and bounded signed-int32 pointee metadata as eligible for the already-existing frame-zero `DW_OP_fbreg` snapshot path.
- Type recovery reuses canonical `selected_inline_direct_structure_type()` and `resolve_bounded_direct_structure_member_type()`; no frame-zero-specific pointer descriptor is introduced.
- Runtime ownership reuses the physical crash frame's compiler-proven frame base and immutable snapshot-memory reader. Selecting an inline context creates no register file, CFA, stack, or memory image.
- Materialization reuses the existing generic snapshot structure decoder. The aggregate/member value preserves its physical immutable storage provenance.
- `CoreInspectionSession::inspect_aggregate_member()` and `dereference_aggregate_member()` reuse the context-neutral Phase 45 traversal primitives. No Phase-61-only member selector or dereference engine exists.
- API evidence proves `inline_typed.linked` remains a typed pointer with bounded signed-int32 pointee metadata and that exactly one dereference yields `0x02468ace` with the pointee read's own immutable snapshot provenance.
- Real `mdbg-core` proves `inline <active> -> print inline_typed -> aggregate-member inline_typed linked -> deref-aggregate-member inline_typed linked -> inline physical`, including inline ownership invalidation.
- Permanent normal GCC / Clang-large CI and dedicated GCC / Clang compiler-oracle/core/session/CLI evidence are green on production head `5261d4df41c30d7f382e148144e5184b0549724d`.
- The candidate is based directly on main `6b22c17576825c85c9ba5696cc40aff9286f65ee`, ahead five / behind zero before the phase seal, with no forbidden AI/bot attribution trailers.

Phase 61 is sealed here. Additional pointer fields, pointer offsets, pointee widths, pointer chains, pointer arithmetic, implicit dereference, null fabrication, mutation, recursive graphs, and arbitrary expressions are not new milestones.

## Phase 62 promotion — frame-zero selected-inline enum-valued aggregate composition

The next meaningful executable composition gap is symbolic enum identity inside a frame-zero selected-inline structure. Phase 52 proves canonical enum roots and symbolic lookup, Phase 53 proves enum-valued aggregate members in historical physical structures, and Phase 56/59/60/61 prove multiple frame-zero selected-inline structure ownership paths. Those capabilities have not yet been composed into a frame-zero enum-valued aggregate.

The first coherent Phase 62 slice must:

1. Start from genuine optimized GCC and Clang frame-zero inline artifacts containing one bounded structure with an enum-valued direct member and a finite compiler-described enumerator table. Do not force a register/stack representation merely to match an existing evaluator.
2. Independently prove lexical ownership, exact structure byte extent, enum-member name/type/offset, enum width/signedness, exact enumerator names/raw values, crash-PC-active location expression, and physical ownership before product assertions run.
3. Follow the compiler-produced ownership form actually emitted by both permanent lanes. Reuse the already-proven frame-zero register-piece or `DW_OP_fbreg` machinery; extend only the minimal evidence-required shape gate.
4. Reuse canonical `resolve_bounded_enum_type()`, direct-structure member typing, immutable structure materialization, context-neutral aggregate-member selection, and conservative `local_enum_symbol()` lookup. No frame-zero-specific enum parser or symbolic renderer is allowed.
5. Preserve exact aggregate/member provenance. Unknown raw enum values must remain numeric and duplicate raw aliases must remain symbolically ambiguous.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <aggregate> -> aggregate-member <aggregate> <enum-member>`, including symbolic + numeric rendering and physical/inline invalidation.
7. Keep enum arithmetic, flags decomposition, mutation, recursive graphs, arbitrary expressions, enum arrays/unions, and unsupported ownership forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion adds a new symbolic typed-value composition at frame zero rather than farming pointer-member variants.
