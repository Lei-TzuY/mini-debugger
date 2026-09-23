# Phase 66 Roadmap — compiler-proven frame-zero selected-inline enum root stack ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 66 closes the remaining enum-root ownership asymmetry. Canonical enum identity, symbolic lookup, historical physical `DW_OP_fbreg` enum roots, and frame-zero register-owned selected-inline enums already existed; this phase proves the same canonical enum model when an active frame-zero selected-inline root is genuinely stack-owned through compiler-produced `DW_OP_fbreg`.

## Completed acceptance

- A dedicated optimized fixture retains one active four-byte unsigned `inline_fbreg_mode` of real compiler enum type `FrameZeroInlineFbregMode` with finite values `Idle=3`, `Ready=7`, and `Busy=42`.
- The crash assembly consumes the enum through a memory operand, retaining genuine stack ownership instead of forcing a synthetic DWARF expression.
- An independent readelf oracle proves the exact selected-inline lexical chain, real `DW_TAG_enumeration_type`, four-byte unsigned representation, exact enumerator table, crash probe PC, active binding, and one exact compiler-produced `DW_OP_fbreg` location before product assertions run.
- GCC and Clang permanent lanes independently retain the enum through frame-zero `DW_OP_fbreg`; the oracle rejects register/register-piece ownership for this milestone.
- Early evidence heads exposed only oracle presentation issues from readelf indirect-string prefixes. Exact red head `679c5e83f83d3e117f132f56b1bea6019eff742c` proves the genuine compiler-owned enum and then fails only at the product ownership seam with `frame-zero selected-inline value requires one exact compiler-proven register operation`.
- Production head `4fd6f3a6e694ab06becc268afe1b59e3a89ef889` extends `frame_zero_fbreg_value_eligible()` only for the canonical bounded four-byte unsigned enum-root shape: no aggregate/array metadata, matching root `LocalEnumType`, non-empty compiler type name, and a non-empty already-bounded enumerator table.
- The selected-inline value path then reuses the existing physical frame-zero frame-base computation, checked `DW_OP_fbreg` address evaluation, immutable snapshot-memory reader, canonical `resolve_bounded_enum_type()`, root `LocalValueType::enum_type`, generic scalar snapshot materialization, and conservative `local_enum_symbol()` lookup.
- No frame-zero-specific enum parser, materializer, symbol table, memory reader, CFA, or stack model is introduced.
- API evidence proves `inline <active> -> inspect inline_fbreg_mode` yields `LocalValueKind::Enumeration`, raw value `42`, exact `FrameZeroInlineFbregMode` metadata, symbol `FrameZeroInlineFbregBusy`, `SnapshotCoreMemory` provenance, numeric fallback for unknown values, alias ambiguity rejection, and invalidation on physical-frame reselection.
- Real `mdbg-core` exercises `inline <active> -> print inline_fbreg_mode -> inline physical` and renders symbolic plus numeric identity with the four-byte unsigned enum representation.
- The new fixture/oracle/integration is permanently wired into both normal CI and the dedicated GCC / Clang compiler-evidence workflow.
- Exact production head `4fd6f3a6e694ab06becc268afe1b59e3a89ef889` passes full GCC / Clang-large CI plus both dedicated GCC / Clang selected-inline evidence lanes, based directly on main `e108728f8032d85ce2390ebd3c8667b3c5f9872d`.

Phase 66 is sealed here. More enumerators, alternate values, aliases, signed variants, additional fbreg offsets, or compiler spelling variants are not new milestones.

## Phase 67 promotion — compiler-proven frame-zero selected-inline pointer stack ownership

The next executable ownership gap is pointer-valued stack state. Frame-zero selected-inline pointers are already compiler-proven and executable when register-owned, historical caller-frame selected-inline pointers already support `DW_OP_fbreg`, and canonical pointer metadata plus one-hop immutable dereference are mature. What remains unproven is a genuine active frame-zero selected-inline pointer root whose pointer bytes are stack-owned through `DW_OP_fbreg`.

The first coherent Phase 67 slice must:

1. Start from a genuine optimized GCC and Clang crash-frame artifact where one active selected-inline pointer to a bounded signed-int32 pointee is retained through compiler-produced frame-zero `DW_OP_fbreg`. Do not synthesize DWARF or force an artificial debug location.
2. Independently prove inline lexical ownership, real x86-64 `DW_TAG_pointer_type`, exact pointer width, signed four-byte integer pointee metadata, crash PC, active location range, exact `DW_OP_fbreg` ownership, non-null pointer value, and immutable pointee value before product assertions run.
3. Reuse canonical pointer root metadata, the physical frame-zero frame-base computation, immutable snapshot-memory reader, and existing context-neutral one-hop pointer dereference. No frame-zero-specific pointer descriptor or dereference engine is allowed.
4. Extend the frame-zero fbreg eligibility boundary only as far as the independently proven bounded pointer-root shape requires.
5. Preserve the distinction between stack-memory provenance for the pointer object and immutable core/runtime-artifact provenance for the pointee.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <pointer> -> deref <pointer>`, plus deterministic invalidation after returning to physical selection.
7. Keep null-pointer policy changes, pointer arithmetic, pointer chains, implicit dereference, mutation, arbitrary register expressions, recursive graphs, and unsupported location forms out of scope.
8. Require full GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion adds a new compiler-proven ownership form to an already-mature canonical pointer capability instead of farming additional enum variants.
