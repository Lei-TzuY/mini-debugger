# Phase 67 Roadmap — compiler-proven frame-zero selected-inline pointer stack ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 67 closes the remaining frame-zero selected-inline pointer ownership asymmetry. Register-owned frame-zero pointers already existed, historical caller-frame selected-inline pointers already supported `DW_OP_fbreg`, and canonical pointer metadata plus one-hop immutable dereference were already mature. This phase proves the same pointer model when the active frame-zero inline pointer object itself is stack-owned through compiler-produced `DW_OP_fbreg`.

## Completed acceptance

- A dedicated optimized fixture retains one active `int32_t *inline_fbreg_pointer` in the selected frame-zero inline scope, pointing at a real signed-int32 object with value `0x13579bdf`.
- The crash assembly consumes the pointer through both a memory operand and the faulting write, keeping the pointer object genuinely stack-resident and live at the exact crash probe without hand-authored DWARF.
- An independent readelf oracle proves the exact selected-inline lexical chain, real x86-64 `DW_TAG_pointer_type`, eight-byte pointer width, signed four-byte integer pointee type, active crash PC, exact compiler-produced `DW_OP_fbreg` ownership, and non-zero pointee symbol before product assertions run.
- Both permanent compiler lanes independently prove genuine stack ownership:
  - GCC: `DW_OP_fbreg -32` for PIE and non-PIE.
  - Clang: `DW_OP_fbreg -8` for PIE and non-PIE.
  No register/register-piece form is accepted by the Phase 67 oracle.
- Exact test-first head `774528f3b666b17de7d901b4665c73b7f90927bd` leaves production unchanged, passes the new GCC/Clang pointer DWARF oracle, preserves existing typed-value evidence, and then fails exactly at the previous frame-zero dispatcher seam with `frame-zero selected-inline value requires one exact compiler-proven register operation`.
- Production head `c294a960f8c39f392764e8454e5ba8c72be308f7` extends only the frame-zero fbreg eligibility boundary. The gate now requires an eight-byte unsigned pointer root with no aggregate/array/enum metadata and an already-resolved bounded signed-int32 pointee descriptor; it does not admit arbitrary pointer roots by kind alone.
- The selected-inline value path reuses the existing physical frame-zero frame-base computation, checked `DW_OP_fbreg` offset evaluation, immutable snapshot-memory reader, canonical pointer root/pointee metadata, generic scalar snapshot materialization, and existing context-neutral one-hop pointer dereference.
- No frame-zero-specific pointer parser, descriptor, stack model, memory reader, CFA, or dereference engine is introduced.
- API evidence proves `inline <active> -> inspect inline_fbreg_pointer -> dereference inline_fbreg_pointer`, exact x86-64 pointer width, bounded signed-int32 pointee metadata, non-null pointer value, exact `0x13579bdf` pointee value, `SnapshotCoreMemory` provenance for the stack-owned pointer object, and immutable core/runtime-artifact provenance for the pointee.
- Real `mdbg-core` exercises `inline <active> -> print inline_fbreg_pointer -> deref inline_fbreg_pointer -> inline physical` and proves physical reselection invalidates stale inline ownership.
- The new fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and the dedicated GCC / Clang compiler/core/session/CLI evidence workflow.
- Exact production head `c294a960f8c39f392764e8454e5ba8c72be308f7` passes full GCC / Clang-large CI plus both dedicated GCC / Clang selected-inline evidence lanes, based directly on main `653753c8c061ee71c710d479649b775e9180693c`.

Phase 67 is sealed here. More pointer offsets, pointer widths, pointer arithmetic, pointer chains, null-policy variants, alternate pointee values, register-expression variants, or implicit dereference are not new milestones.

## Phase 68 promotion — compiler-proven frame-zero selected-inline floating stack ownership

The next meaningful executable gap is a different source-value kind rather than another pointer shape. Snapshot floating scalars already have canonical `LocalValueKind::Floating` type recovery, immutable bit-pattern storage, and deterministic CLI rendering. Frame-zero physical floating ownership is already compiler-proven through XMM state. What remains unsupported is an active selected-inline floating root whose bytes are genuinely stack-owned through compiler-produced frame-zero `DW_OP_fbreg`.

The first coherent Phase 68 slice must:

1. Start from a genuine optimized GCC and Clang crash-frame artifact where one active selected-inline `float` or `double` root is retained through compiler-produced frame-zero `DW_OP_fbreg`. Do not hand-author the expression or force a synthetic location.
2. Independently prove inline lexical ownership, real `DW_TAG_base_type` with `DW_ATE_float`, exact 4/8-byte width, crash PC, active location range, exact `DW_OP_fbreg` ownership, and deterministic floating value before product assertions run.
3. Reuse the existing floating `LocalValueType`, physical frame-zero frame-base computation, immutable snapshot-memory reader, raw IEEE-754 bit preservation, and existing floating CLI renderer. No selected-inline-specific floating parser/materializer is allowed.
4. Extend selected-inline type validation and frame-zero fbreg eligibility only as far as the independently proven bounded floating scalar shape requires.
5. Preserve stack-memory provenance for the floating object and existing thread/frame/inline invalidation.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <floating>`, including exact decoded value and representation.
7. Keep vector/SIMD aggregates, x87, long double, NaN canonicalization changes, arithmetic, mutation, arbitrary expressions, unsupported register locations, and new floating widths out of scope.
8. Require full GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion advances a genuinely different typed-value ownership form while reusing the mature immutable snapshot machinery.
