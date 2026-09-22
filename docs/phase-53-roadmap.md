# Phase 53 Roadmap — physical enum identity through an aggregate-member boundary

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 53 proves that compiler-described enum identity composes through an already-supported historical physical structure boundary instead of working only as a top-level enum root.

## Completed acceptance

- The permanent historical caller fixture retains a genuine stack-resident eight-byte `caller_enum_aggregate` with signed 32-bit `direct = 0x31415926` at offset 0 and `CallerPhysicalMode mode = CallerPhysicalBusy` at offset 4.
- An independent readelf oracle proves GCC and Clang retain the outer structure through the already-supported historical `DW_OP_fbreg` path, preserve exact outer size/member offsets, resolve `mode` to the real four-byte `DW_TAG_enumeration_type`, and preserve the finite `CallerPhysicalIdle = 3`, `CallerPhysicalReady = 7`, `CallerPhysicalBusy = 42` table before product assertions run.
- Test-first head `a5459bd92a7d3ed831da6ae8ffcaf28c1f16537e` keeps the existing 54-test suite and dedicated selected-inline evidence intact, then fails under both GCC and Clang at the intended type-composition boundary because the ordinary physical structure-member resolver still falls through to integer-only resolution for an enum member.
- Production head `f13d7a0cc4a736b6fe68434954f9550a5b0ecd74` extends the ordinary canonical structure-member path to recognize an already-canonical `resolve_bounded_enum_type()` result. No new enum parser, descriptor, or physical-only materializer is introduced.
- The resolved member reuses `LocalStructMemberType::enum_type`, `LocalValueKind::Enumeration`, the generic immutable structure materializer, and the existing context-neutral aggregate-member selector.
- Selecting `caller_enum_aggregate.mode` preserves exact raw value 42, `CallerPhysicalMode` metadata, symbolic lookup to `CallerPhysicalBusy`, containing aggregate provenance, and deterministic thread/frame invalidation.
- Real `mdbg-core` exercises `frame 1 -> inline physical -> print caller_enum_aggregate -> aggregate-member caller_enum_aggregate mode` and renders the enum member symbolically and numerically.
- Mutable live-process structure decoding explicitly rejects enum-valued members, so sharing the canonical type resolver does not silently broaden live ptrace semantics without matching evidence.
- Exact production head `f13d7a0cc4a736b6fe68434954f9550a5b0ecd74` passes full GCC / Clang-large CI plus both GCC / Clang selected-inline regression/evidence lanes.

Phase 53 is sealed here. More enum-valued structure examples, alternate enumerator values, enum arrays/unions, signed enum variants, or extra aliases are not new milestones.

## Phase 54 promotion — canonicalize bounded direct-structure member typing

Phase 53 exposes a structural debt rather than another missing enum variant: ordinary `resolve_value_type()` and `resolve_snapshot_struct_member_type()` still maintain overlapping direct-structure member type logic for integer, pointer, enum, wrapper traversal, offset bounds, and metadata attachment.

The next coherent architectural slice must:

1. Introduce one ownership-neutral bounded direct-structure member resolver in the shared DWARF layer.
2. Make ordinary physical root structures, selected-inline direct structures, and snapshot structure pointees consume that same resolver where their supported type surface overlaps.
3. Preserve the already-proven special handling boundaries for bit fields and bounded one-hop nested structures without creating recursive graphs or widening unsupported live semantics.
4. Delete duplicated pointer/integer/enum wrapper traversal and duplicate offset/bounds validation instead of wrapping one implementation around another while keeping both.
5. Keep emitted `LocalStructMemberType` metadata byte-for-byte equivalent for existing integer, pointer, enum, bit-field, and nested-structure tests; add focused equivalence/regression evidence if needed.
6. Preserve explicit live fail-closed guards for bit-field and enum-valued members where current live-process evidence is absent.
7. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler/core/session/CLI evidence before integration.

This promotion improves architecture depth and lowers future type-composition risk instead of farming additional enum-member variants.
