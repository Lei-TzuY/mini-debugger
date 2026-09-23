# Phase 65 Roadmap — compiler-proven frame-zero selected-inline union stack ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 65 closes the frame-zero selected-inline union ownership asymmetry between register-owned and stack-owned objects. Phase 58 already proved canonical bounded union semantics when the compiler owns the active inline bytes in RDX/RCX; this phase proves the same value model when a genuine optimized compiler retains the active union through frame-zero `DW_OP_fbreg`.

## Completed acceptance

- A dedicated optimized fixture retains one active four-byte `inline_fbreg_union` in the selected frame-zero inline scope with overlapping signed 32-bit `signed_value` and unsigned 32-bit `unsigned_value`.
- An independent readelf oracle proves the exact inline lexical chain, real `DW_TAG_union_type`, four-byte extent, exactly two direct offset-zero integer members with signed/unsigned compiler encodings, crash probe PC, and one exact compiler-produced `DW_OP_fbreg` location before product assertions run.
- GCC and Clang permanent lanes independently retain the union through direct frame-zero `DW_OP_fbreg -4`. The fixture does not force register ownership and the oracle rejects register/register-piece forms.
- Test-first exact head `4ea05ba54344393991568a634bf9b79ff381d348` keeps the established typed-value suite intact, proves the genuine compiler-owned stack union, and then fails only at the intended dispatcher seam with `4-byte union register requires compiler-proven RDX or RCX ownership`.
- Production head `476f785c47bf51bf74c7f2d17faef95dac437f33` extends `frame_zero_fbreg_value_eligible()` only for the independently proven bounded union shape: exact four-byte storage, exactly two named offset-zero integer members, signed int32 `signed_value`, unsigned uint32 `unsigned_value`, and no pointer/bit-slice/enum/nested metadata.
- The selected-inline value path then reuses the existing physical frame-zero frame-base computation, checked `DW_OP_fbreg` address evaluation, immutable snapshot-memory reader, canonical bounded union resolver, generic union byte materialization, context-neutral explicit union-member selector, and provenance model.
- No frame-zero-specific union descriptor, parser, materializer, member selector, memory reader, CFA, stack model, or active-member inference is introduced.
- Existing Phase 58 register-owned union behavior remains on the register-piece path. Unsupported union shapes, nested unions, discriminators/variant parts, arbitrary reinterpretation, mutation, recursive graphs, and unsupported location forms remain fail-closed.
- API evidence proves the exact raw value `0x55667714` under both signed and unsigned explicit member views, deterministic missing-member rejection, `SnapshotCoreMemory` provenance, and invalidation when returning to the physical frame.
- Real `mdbg-core` exercises `inline <active> -> print inline_fbreg_union -> union-member inline_fbreg_union signed_value -> union-member inline_fbreg_union unsigned_value`, plus deterministic missing-member rejection and `inline physical` invalidation.
- The new fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and the dedicated GCC / Clang compiler-evidence workflow.
- Exact production head `476f785c47bf51bf74c7f2d17faef95dac437f33` passes full GCC / Clang-large CI plus both dedicated compiler/core/session/CLI evidence lanes and is based directly on main `0070d49555df41e3d6c40d4d70b7ab92f9dbb114`, ahead two / behind zero before the phase seal.

Phase 65 is sealed here. More union members, alternate scalar widths, alternate `DW_OP_fbreg` offsets, additional overlapping layouts, or compiler spelling variants are not new milestones.

## Phase 66 promotion — compiler-proven frame-zero selected-inline enum root stack ownership

The next executable ownership gap is a distinct value kind. Canonical enum root identity and symbolic lookup are mature; historical physical `DW_OP_fbreg` enum roots are proven, and frame-zero selected-inline enum register ownership is already proven. What remains unproven is a genuine active frame-zero selected-inline enum root whose scalar bytes are stack-owned through `DW_OP_fbreg`.

The first coherent Phase 66 slice must:

1. Start from a genuine optimized GCC and Clang crash-frame artifact where one active selected-inline enum root is retained through compiler-produced frame-zero `DW_OP_fbreg`. Do not synthesize DWARF or force an artificial location expression.
2. Independently prove inline lexical ownership, real `DW_TAG_enumeration_type`, exact byte width and signedness/underlying representation, finite enumerator table, crash PC, active location range, and exact `DW_OP_fbreg` ownership before product assertions run.
3. Reuse canonical `resolve_bounded_enum_type()`, `LocalValueType::enum_type`, the physical frame-zero frame-base computation, immutable snapshot-memory reader, generic scalar snapshot materialization, and conservative symbolic lookup. No frame-zero-specific enum parser/materializer/symbol table is allowed.
4. Extend the frame-zero fbreg eligibility boundary only as far as the independently proven enum root shape requires. Unknown raw values must remain numeric and duplicate raw aliases must remain symbolically ambiguous.
5. Preserve exact stack-memory/runtime-artifact provenance and selected-inline lexical/frame/thread invalidation.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <enum>`, including symbolic + numeric rendering and physical-selection invalidation.
7. Keep enum arithmetic, flags decomposition, mutation, scoped-name reconstruction, arbitrary register expressions, recursive graphs, and unsupported location forms out of scope.
8. Require full GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion advances a new compiler-proven machine-state ownership form for a different canonical value kind rather than farming additional union layouts.
