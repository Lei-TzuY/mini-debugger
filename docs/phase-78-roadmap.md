# Phase 78 Roadmap — compiler-proven current-frame live bit-field structures

Status: complete for the current Linux x86-64 optimized live-debugger milestone.

Phase 78 closes the current-frame live bit-field materialization gap. Canonical bit-field type recovery and checked bit extraction already existed for immutable physical/selected-inline structures; this phase proves the same compiler-described semantics for a genuine stopped live process.

## Completed acceptance

- The permanent optimized live fixture now retains one genuine four-byte `LiveBitFields` local in `inspect_live_bit_fields`, with signed five-bit `signed_bits = -7` and unsigned six-bit `unsigned_bits = 41`.
- An independent readelf oracle proves GCC and Clang both retain the live structure through compiler-produced `DW_OP_fbreg` ownership at `live_bit_field_probe`, with exact four-byte aggregate extent, exactly two direct members, signed/unsigned four-byte base types, exact bit widths 5/6, and exactly one compiler bit-location spelling per member.
- The oracle preserves the already-proven little-endian x86-64 interpretation of GCC legacy `DW_AT_bit_offset` and Clang `DW_AT_data_bit_offset`, requiring aggregate-relative bit positions 0 and 5. It does not infer packing from the C declaration.
- Test-first head `41d2f55843720533f6d52d8c93dd3f59c22430d5` keeps the dedicated compiler evidence green under both GCC and Clang and fails both normal compiler lanes only at the intended product guard: `live bit-field structure materialization is outside current compiler evidence`.
- Production head `9c5cedd87582cb5230d42a7668ddd98dab3d8f37` removes no type or location checks. The shared live structure decoder now consumes the already-canonical `LocalBitSlice` through `decode_bounded_bit_field()`; ordinary byte-aligned members still use `decode_integer()`.
- Signed five-bit `-7` normalizes to the existing four-byte signed representation `0xfffffff9`; unsigned six-bit `41` remains `41`. Malformed/out-of-bounds slice metadata continues to fail in the shared checked decoder.
- The live enum-valued structure guard remains fail-closed; this phase does not silently broaden unrelated typed-member semantics.
- `inspect_local_aggregate_member()` is reused unchanged for explicit signed/unsigned member selection.
- The live CLI now exposes `aggregate-member <name> <member>` over the existing context-neutral selector. It renders bounded integer, pointer, and enum scalar leaves without introducing a new traversal engine; non-scalar direct members remain rejected by this command.
- Real API and CLI evidence proves `break live_bit_field_probe -> continue -> print live_bit_fields -> aggregate-member live_bit_fields signed_bits -> aggregate-member live_bit_fields unsigned_bits`, exact values, and clean process completion.
- Exact production head `9c5cedd87582cb5230d42a7668ddd98dab3d8f37` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler-evidence lanes, based directly on main `5bae98e1b307f080976437efb83c9c13c0b11196`.

Phase 78 is sealed here. More bit widths, alternate packing examples, endian variants without independent evidence, nested bit-field aggregates, writes, arbitrary bit slicing, and mutation are not new milestones.

## Phase 79 promotion — compiler-proven current-frame live enum-valued structure members

The next meaningful executable gap is already explicit in the live structure decoder: canonical direct enum-member typing and symbolic enum identity exist across selected-inline and historical physical ownership, but current-frame live structure materialization still rejects `LocalValueKind::Enumeration` / `enum_type` members.

The first coherent Phase 79 slice must:

1. Start from a genuine optimized GCC/Clang current-frame live structure containing one ordinary integer direct member and one enum-valued direct member, retained through a compiler-produced location supported by the existing live frame-base machinery.
2. Independently prove the outer structure DIE, exact extent/member offsets, enum member type, enum byte width/signedness, finite enumerator table, stop PC, lexical ownership, and actual location form before product assertions run.
3. Reuse canonical `resolve_bounded_enum_type()`, `LocalStructMemberType::enum_type`, the existing live structure bytes, and conservative `local_enum_symbol()`; do not add a live-only enum parser or descriptor.
4. Extend `decode_structure()` only enough to materialize the already-bounded enum member from its owned bytes while preserving `LocalValueKind::Enumeration`, exact raw value, and enum metadata.
5. Reuse live `print` plus the Phase-78 `aggregate-member` command to render the enum member symbolically and numerically. Unknown raw values remain numeric and duplicate raw aliases remain ambiguous.
6. Keep nested enum aggregates, arrays/unions of enums, enum writes, flags decomposition, casts, arithmetic, recursive object graphs, and unsupported location forms out of scope.
7. Require full GCC / Clang-large CI plus independent compiler oracle/API/CLI evidence on the exact candidate before integration.

This promotion adds a new live typed-member capability rather than extending Phase 78 with cosmetic variants.
