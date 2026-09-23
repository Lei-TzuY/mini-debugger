# Phase 68 Roadmap — compiler-proven frame-zero selected-inline floating stack ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 68 closes the selected-inline floating stack-ownership gap. Frame-zero physical floating values already had canonical DW_ATE_float recovery and XMM ownership, while the selected-inline frame-zero fbreg path already handled bounded stack-owned aggregates, arrays, unions, enums, and pointers. This phase proves one genuine stack-owned selected-inline double and routes it through the existing immutable snapshot-memory machinery.

## Completed acceptance

- A dedicated optimized fixture retains one active `double inline_fbreg_floating = 1234.25` in the selected frame-zero inline scope.
- The fixture forces genuine stack addressability with a compiler memory operand while also consuming the same value as the source of the faulting floating store; no hand-authored DWARF location is injected.
- An independent readelf oracle proves the exact inline lexical chain, real `DW_TAG_base_type`, exact eight-byte width, `DW_ATE_float`, crash probe PC, and one exact compiler-produced `DW_OP_fbreg` active location before product assertions run.
- Both permanent GCC and Clang lanes independently pass that oracle for PIE and non-PIE artifacts.
- Exact test-first head `a4df371aef7a1916a8003ee9b53edab8cc9e3aec` leaves production unchanged, passes the new compiler oracle, and then fails under GCC and Clang only at the intended selected-inline type seam with `local base type is not a supported signed/unsigned integer`.
- Production head `5757d2a4418d4b11448d35100b8c743d14a2e8ab` reuses the mature `resolve_snapshot_floating_type()` before pointer/aggregate dispatch, matching the ordinary snapshot path instead of creating a selected-inline floating parser.
- Frame-zero fbreg eligibility is widened only for the independently proven exact shape: `LocalValueKind::Floating`, eight-byte `double`, unsigned metadata flag, no members, array metadata, enum metadata, or pointee metadata.
- The value then reuses the existing physical frame-base computation, checked fbreg address evaluation, immutable `read_snapshot_memory()`, generic `materialize_snapshot_memory_value()`, IEEE-754 raw-bit preservation, and existing floating CLI renderer.
- API evidence proves exact `1234.25` recovery, `SnapshotCoreMemory` provenance, frame-zero ownership, and invalidation when the physical frame is reselected.
- Real `mdbg-core` exercises `inline <active> -> print inline_fbreg_floating -> inline physical` and renders `1234.25 [8-byte floating] [value-core]`.
- The new fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and dedicated GCC / Clang compiler/core/session/CLI evidence.
- Exact production head `5757d2a4418d4b11448d35100b8c743d14a2e8ab` passes both full push workflow families, based directly on main `4032964895d8fdd6648bf6431b8aed7ecb3aa76d`.

Phase 68 is sealed here. A four-byte float variant, alternate constants, extra fbreg offsets, NaN/Inf variants, XMM/register variants, long double, x87, SIMD/vector values, arithmetic, or mutation are not new milestones.

## Phase 69 promotion — canonicalize selected-inline root type resolution

Phase 68 exposes an architectural debt shared by many recent typed-value phases. `inspect_inline_scalar_unit()` currently maintains a hand-written priority chain for floating, pointer, direct structure, union, fixed array, enum, then ordinary scalar fallback. Each new independently proven root kind has required another condition in this dispatch, even though the resulting `LocalValueType` and metadata are already canonical.

The next coherent architectural slice must:

1. Introduce one ownership-neutral selected/snapshot root-type resolution result that returns the canonical `LocalValueType` plus any bounded pointer-pointee metadata required by existing one-hop dereference.
2. Preserve the exact current supported surface and precedence for 4/8-byte floating scalars, bounded integer/pointer roots, direct structures, unions, fixed arrays, enums, and existing wrapper traversal. This phase must not add a new source type merely because the resolver is being consolidated.
3. Make selected-inline inspection and ordinary snapshot inspection consume the shared root resolver where their supported type surface overlaps; delete the duplicated floating-before-pointer/root-kind dispatch rather than wrapping both old implementations.
4. Keep selected-inline-only direct-structure semantics bounded where necessary, but move ownership-neutral type choice out of the inline evaluator.
5. Preserve pointer pointee metadata, enum metadata, array descriptors, union/structure members, floating identity, all existing provenance, and all fail-closed location-expression boundaries byte-for-byte.
6. Add focused resolver-equivalence regression evidence if needed, but use the full existing GCC / Clang-large and dedicated GCC / Clang genuine-artifact suites as the primary proof that no typed-value capability regressed.
7. Do not turn this into recursive type graphs, generalized C/C++ expression evaluation, arbitrary pointee expansion, or a generic DWARF type system.
8. Require full push and PR-event GCC / Clang-large CI plus dedicated compiler/core/session/CLI evidence on the exact candidate before integration.

This promotion consolidates the root type architecture before another executable type frontier is added, preventing future capability growth from becoming another conditional ladder.
