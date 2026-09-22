# Phase 55 Roadmap — compiler-proven frame-zero selected-inline pointer ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 55 closes the frame-zero selected-inline pointer gap without inventing inline machine state or broadening the register-expression surface beyond genuine compiler evidence.

## Completed acceptance

- The permanent optimized frame-zero inline fixture now retains a genuine signed-int32 pointee with value `0x13579bdf` and an active `int* inline_pointer` inside `inline_inner`.
- The crash assembly consumes `inline_pointer`, making the source pointer genuinely live at the faulting instruction rather than relying on a zero-length or dead debug location.
- The independent DWARF oracle resolves the selected-inline lexical binding, proves a real x86-64 `DW_TAG_pointer_type`, verifies a signed four-byte integer pointee, resolves the location list at the exact crash probe PC, and requires one compiler-produced direct register location.
- GCC and Clang independently prove different already-supported register forms at the crash PC:
  - GCC: `DW_OP_reg2 (rcx)`
  - Clang: `DW_OP_reg1 (rdx)`
  Both have non-empty active location ranges covering the probe.
- Earlier evidence-only heads intentionally failed before production changed: the first fixture attempt exposed dead/non-active pointer locations; after keeping the pointer live and parsing the PC-active location-list entry, exact red head `e94b135bfd23a7dfb5ebba9bed34f469b421922e` passes the compiler oracle under both GCC and Clang and then fails only at the explicit product guard `frame-zero selected-inline pointer materialization is outside current compiler evidence`.
- Production head `07f8b1636f795dd5f43fa5af948447a507144a50` removes only that pointer guard. It reuses the existing frame-zero `DW_OP_reg1` / `DW_OP_reg2` register ownership, existing canonical pointer type/pointee metadata, and existing context-neutral one-hop dereference path.
- No generic `DW_OP_regN` widening, second pointer descriptor, inline register file, inline CFA, or new dereference engine is introduced.
- The pointer object retains `SnapshotCoreRegister` ownership. Dereferencing reads the pointee from immutable snapshot memory/runtime-artifact storage and therefore preserves the distinction between register-owned pointer bytes and memory-owned pointee bytes.
- Real API evidence proves selected frame-zero `inline_inner -> inspect inline_pointer -> dereference inline_pointer`, exact x86-64 pointer width, signed-int32 pointee metadata, non-null pointer value, exact `0x13579bdf` pointee value, and immutable provenance.
- Real `mdbg-core` executes `inline 1 -> print inline_pointer -> deref inline_pointer` against the genuine core.
- Full pre-existing 54-test CTest coverage remains green under both normal GCC and Clang-large lanes.
- Exact implementation head `07f8b1636f795dd5f43fa5af948447a507144a50` passes normal GCC / Clang-large CI plus both dedicated GCC / Clang selected-inline evidence lanes.

Phase 55 is sealed here. Additional register numbers, pointer widths, pointer arithmetic, pointer chains, implicit dereference, mutation, or speculative compiler expressions are not extensions of this milestone.

## Phase 56 promotion — compiler-proven frame-zero selected-inline direct structure ownership

The next executable gap is another explicit frame-zero guard, but at the typed-object boundary rather than another pointer variant. Caller-frame selected-inline direct structures already use the canonical bounded structure type and immutable materializer, while frame-zero selected-inline structures are still rejected before those shared primitives can consume genuine register-owned compiler state.

The first coherent Phase 56 slice must:

1. Start from a genuine optimized GCC and Clang frame-zero inline artifact containing one small direct structure whose active compiler location at the crash PC is independently proved. Do not synthesize DWARF or assume register packing.
2. Prove exact structure byte extent, direct member names/types/offsets, selected-inline lexical ownership, active crash-PC location expression, and the physical register/memory ownership needed to reconstruct the value before product assertions run.
3. Extend only the compiler-produced location form actually emitted by both permanent lanes. Reuse the canonical `LocalValueType`, direct-member resolver, immutable structure materializer, and existing selected-inline session/CLI surfaces.
4. Preserve physical-frame ownership: inline selection does not create an independent register file, stack pointer, CFA, or memory image.
5. If the artifact requires register-piece composition, reuse/extend the existing bounded register-piece semantics rather than special-casing one structure; if it does not, do not widen to pieces speculatively.
6. Prove exact API/CLI rendering and direct member selection, plus deterministic invalidation after physical frame/thread/inline selection changes.
7. Keep nested structures, arrays, unions, mutation, recursive graphs, arbitrary expressions, and unsupported register forms out of this first frame-zero aggregate slice.
8. Require full GCC / Clang-large CI plus permanent compiler oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion advances a new frame-zero typed-object capability rather than farming pointer variants.
