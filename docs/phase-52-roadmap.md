# Phase 52 Roadmap — compiler-proven physical enum identity and symbolic values

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 52 closes the enum-root asymmetry between selected-inline and historical physical-frame inspection. A historical physical caller can now own one compiler-described enum local through the ordinary root type path while preserving symbolic type identity, finite enumerator metadata, normalized raw value, and immutable snapshot provenance.

## Completed acceptance

- The permanent historical caller fixture retains one genuine stack-resident `CallerPhysicalMode caller_mode = CallerPhysicalBusy` with direct enumerators `CallerPhysicalIdle = 3`, `CallerPhysicalReady = 7`, and `CallerPhysicalBusy = 42`.
- An independent readelf oracle proves GCC and Clang retain `caller_mode` through compiler-produced historical `DW_OP_fbreg` ownership at the caller lookup PC, resolve it to a real four-byte `DW_TAG_enumeration_type`, preserve an unsigned integral representation, and emit exactly the expected direct enumerator table before product assertions run.
- The two compiler lanes retain their genuine representation spellings: direct `DW_AT_encoding` or a referenced compatible base type is accepted only when byte width and signedness agree. No representation is inferred from the C declaration.
- Test-first exact head `366387d399a766f95454e201b50aa201ef507c88` kept the existing 54-test suite green and the dedicated selected-inline evidence green under GCC and Clang, then failed in both normal compiler lanes at the intended architecture boundary: ordinary `resolve_value_type()` did not accept an enumeration root.
- Canonical `kDwTagEnumerationType`, `kDwTagEnumerator`, `kDwAtConstValue`, bounded enumerator count, constant-form validation, raw-value normalization, and `resolve_bounded_enum_type()` now live in the shared DWARF layer. The selected-inline-only enum parser/constants are removed.
- `LocalValueType` now carries optional root `LocalEnumType` metadata, so enum identity is part of the canonical root descriptor rather than post-hoc selected-inline state.
- Ordinary `resolve_value_type()` returns `LocalValueKind::Enumeration` plus the exact bounded enum metadata for a genuine enum root.
- `materialize_snapshot_memory_value()` validates and preserves root enum metadata while decoding immutable scalar bytes. The returned `LocalScalarValue` therefore carries kind, width/signedness, enumerator table, raw value, and exact core/runtime-artifact provenance together.
- Historical physical `DW_OP_fbreg` accepts the bounded enum root. Live ptrace enum materialization, historical live-frame enum materialization, snapshot register enum ownership, `DW_OP_breg3`, and `DW_OP_addr` enum materialization remain explicitly fail-closed because this phase supplies no matching compiler evidence for those machine-state forms.
- Selected-inline direct enum inspection now calls the same canonical resolver. Its caller-frame generic snapshot materializer consumes the same root descriptor, while the already-proven frame-zero selected-inline register enum path preserves canonical enum metadata.
- Snapshot enum-valued structure-member recovery also uses `resolve_bounded_enum_type()`; there is no remaining `selected_inline_enum_type` parser.
- Real API evidence proves `frame 1 -> inline physical -> print caller_mode`: the value is the unsigned four-byte `CallerPhysicalMode`, raw `42`, symbol `CallerPhysicalBusy`, and immutable core provenance. Unknown numeric values stay numeric and duplicate raw aliases remain symbolically ambiguous.
- Real `mdbg-core` renders `CallerPhysicalMode::CallerPhysicalBusy (0x2a)` with `[4-byte enum unsigned]`, and thread/frame changes invalidate the historical local.
- Exact implementation head `7fb6783e1d39066483adb6026c02214df9d33c24` passes full GCC / Clang-large CI plus both GCC / Clang selected-inline regression/evidence lanes.

Phase 52 is sealed here. Additional enumerator counts, alternate positive values, signed enum variants, typedef spelling variants, enum arithmetic, flags decomposition, scoped-name reconstruction, mutation, or another top-level enum do not constitute a new architectural milestone.

## Phase 53 promotion — physical enum identity through an aggregate-member boundary

The next meaningful composition gap is not another enum root. Selected-inline inspection already proves that bounded enum identity can survive one direct structure-member boundary, while the ordinary historical physical structure resolver still recognizes direct integer, pointer, nested-structure, and bit-field members but does not preserve an enum-valued direct member.

The first coherent Phase 53 slice must:

1. Start from genuine GCC and Clang historical caller cores where one stack-resident physical structure contains one ordinary scalar member plus one enum-valued direct member and is retained through an already-supported compiler-produced `DW_OP_fbreg` location. Do not synthesize DIEs or inject member values.
2. Independently prove the outer structure DIE, exact byte extent, direct member names/constant offsets, the enum member's real `DW_TAG_enumeration_type`, four-byte integral representation, and finite direct enumerator table before product assertions run.
3. Extend the ordinary canonical structure-member resolver to recognize the already-canonical `resolve_bounded_enum_type()` result rather than adding a physical-only enum-member parser or another member descriptor.
4. Reuse `LocalStructMemberType::enum_type`, generic immutable structure materialization, and the context-neutral aggregate-member selector. Selecting the enum member must preserve `LocalValueKind::Enumeration`, exact enum metadata, raw value, symbolic lookup, and containing aggregate provenance.
5. Prove real `mdbg-core` behavior for `frame 1 -> inline physical -> print <aggregate> -> aggregate-member <aggregate> <enum-member>`, rendering symbolic + numeric enum identity and deterministic frame/thread invalidation.
6. Keep live ptrace enum-valued aggregate expansion, enum arrays, nested enum-bearing recursive structures, unions of enums, mutation, expression parsing, type units/templates, and ABI guesses out of scope.
7. Require full GCC / Clang-large CI plus the relevant permanent GCC / Clang compiler/oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion tests whether canonical symbolic type identity composes through an already-supported historical physical aggregate boundary instead of farming additional enum roots.
