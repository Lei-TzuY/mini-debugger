# Phase 63 Roadmap — canonical frame-zero fbreg structure eligibility

Status: complete for the current Linux x86-64 bounded typed-value architecture milestone.

Phase 63 removes the shape-switch duplication accumulated across Phases 59–62. Frame-zero selected-inline bit-field, one-hop nested, pointer-valued, and enum-valued structures all already use the same physical frame base, immutable snapshot-memory reader, canonical type metadata, and generic structure materializer; only their eligibility bookkeeping remained duplicated.

## Completed acceptance

- One pure `frame_zero_fbreg_structure_eligible(const LocalValueType&)` classifier now owns the bounded type-shape policy for frame-zero selected-inline structure values.
- The classifier preserves exactly the four previously compiler-proven accepted shapes:
  - integer bit-field-only structures with canonical bit-slice metadata;
  - exactly one direct integer scalar plus one one-hop nested structure containing exactly one terminal integer;
  - exactly one offset-zero x86-64 pointer member with bounded signed-int32 pointee metadata;
  - exactly one signed-int32 direct member plus one bounded unsigned 32-bit enum member with non-empty canonical enum identity/table.
- The classifier is ownership-neutral: it reads no registers, frame base, CFA, DWARF location expression, memory, module mapping, or snapshot bytes and performs no materialization.
- The selected-inline value path separately proves frame-zero ownership and `DW_OP_fbreg`; only after that ownership decision does it reuse the existing physical frame-base calculation and immutable snapshot-memory materializer.
- The four ad hoc `frame_zero_*_structure` booleans and their duplicated OR bookkeeping are deleted rather than wrapped.
- No new structure/member form is accepted. Register-piece structures, arrays, unions, scalar registers, pointer registers, caller-frame values, and all unsupported location forms keep their prior paths and failure behavior.
- Permanent Phase 59–62 GCC/Clang compiler-oracle/core/session/CLI evidence remains byte-for-byte behaviorally unchanged.
- Exact implementation head `e05a57dfc6fe21abc961d67eee960ec509c4a899` passes full GCC / Clang-large CI and dedicated GCC / Clang selected-inline evidence. Duplicate workflow deliveries for the same head also complete successfully.
- The candidate is based directly on main `3cfefdd2146ce50a831b44af45698e8ce1563bcb`, ahead one / behind zero before the phase seal.

Phase 63 is sealed here. Adding another structure layout merely to extend the classifier is not a milestone.

## Phase 64 promotion — compiler-proven frame-zero selected-inline fixed-array stack ownership

The next executable ownership gap is outside the structure classifier. Frame-zero selected-inline fixed arrays are already proven when compiler output owns the bytes through bounded RDX/RCX register-piece expressions (Phase 57), and the canonical array type/materializer/indexer is mature. But when a genuine frame-zero inline array is stack-owned through `DW_OP_fbreg`, the current dispatcher does not treat that as an eligible frame-zero stack value and instead falls into the register-piece path.

The first coherent Phase 64 slice must:

1. Start from a genuine optimized GCC and Clang crash-frame artifact where an active selected-inline fixed array is retained through compiler-produced frame-zero `DW_OP_fbreg`. Do not force the location form or hand-author DWARF.
2. Independently prove inline lexical ownership, exact bounded array type/count/element width/signedness, crash PC, active location range, compiler-produced `DW_OP_fbreg` expression, and deterministic element values before product assertions run.
3. Reuse the selected physical frame's existing frame-base computation and immutable snapshot-memory reader. Inline selection remains lexical/type ownership only and must not create an inline CFA, stack, or memory image.
4. Reuse canonical `resolve_bounded_fixed_array_type()`, generic immutable array materialization, context-neutral checked indexing, and exact provenance. No frame-zero-specific array descriptor/materializer/indexer is allowed.
5. Generalize the frame-zero fbreg eligibility boundary only as far as the independently proven array shape requires; do not silently admit unions or arbitrary aggregates without their own evidence.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <array> -> array-element <array> <index>`, deterministic out-of-range rejection, and invalidation after inline/frame/thread selection changes.
7. Keep multidimensional arrays, dynamic bounds, flexible arrays, pointer arithmetic, mutation, arbitrary expressions, and unsupported compiler location forms out of scope.
8. Require full GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion returns immediately from architecture consolidation to a genuine machine-state ownership capability rather than farming more structure-shape variants.
