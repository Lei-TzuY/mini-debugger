# Phase 51 Roadmap — compiler-proven physical bit-field member semantics

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 51 closes the bit-field asymmetry between selected-inline and historical physical-frame structure inspection. A historical physical caller can now own one compiler-described bit-field structure through the ordinary root type path, preserve exact bounded bit-slice metadata, and materialize signed/unsigned sub-byte members from immutable snapshot bytes using the same semantics as selected-inline structures.

## Completed acceptance

- The permanent historical caller fixture now retains a genuine stack-resident four-byte `caller_bit_fields` structure with signed five-bit `signed_bits = -7` and unsigned six-bit `unsigned_bits = 41`.
- An independent readelf oracle proves GCC and Clang both retain the historical variable through compiler-produced `DW_OP_fbreg` ownership, a real four-byte `DW_TAG_structure_type`, exactly two direct integer bit-field members, exact widths 5 and 6, exactly one compiler-emitted bit-location spelling per member, the caller lookup PC, and the existing frame-base form before product assertions run.
- The oracle converts the compiler-described locations to aggregate-relative little-endian x86-64 bit positions and requires `signed_bits` at bit 0 and `unsigned_bits` at bit 5. It does not infer layout from the C source declaration.
- Test-first exact head `97323b4cf04f58ea3626d2c4691a6d6e81ae253b` kept the existing 54-test suite green under both normal compiler lanes and reached the genuine core. GCC then exposed the semantic gap by decoding the signed bit field as a full byte-aligned integer; Clang exposed the complementary resolver gap because its compiler-produced `DW_AT_data_bit_offset` layout has no byte-level member offset.
- One canonical `resolve_bounded_bit_slice()` and `resolve_bounded_bit_field_member_type()` now serve both ordinary physical and selected-inline structure type recovery. The selected-inline-only bit attribute constants and bit-slice resolver are removed.
- The shared resolver preserves only the already-proven bounded integer bit-field model: non-zero width within declared scalar storage, exactly one supported bit-location spelling, checked aggregate bounds, and the existing little-endian Linux x86-64 legacy `DW_AT_bit_offset` translation. Unsupported or contradictory metadata remains fail-closed.
- One canonical `decode_bounded_bit_field()` now performs checked bit extraction, masking, and signed normalization. The selected-inline-only bit mask/decoder is removed.
- `materialize_snapshot_memory_value()` now honors canonical `LocalBitSlice` metadata for immutable structures. The selected-inline-only structure materializer is removed; physical and selected-inline roots use the same structure byte materialization path.
- A migration regression at intermediate head `bf23cd62702df205730808ffd3755a60b0859ff1` proved the old selected-inline flow had supplied only an empty structure shell because its former dedicated materializer carried the full descriptor separately. Exact fix `d8938b6e15bfbf7a5c5c7bce27b8f6a09bb548a1` now forwards the full canonical direct-structure descriptor into the generic materializer, restoring all existing aggregate/member behavior.
- Mutable live ptrace structure decoding explicitly rejects bit-field descriptors. Sharing the compiler type resolver therefore does not silently broaden live mutation/debugger semantics without matching evidence.
- Real API evidence proves `frame 1 -> inline physical -> print caller_bit_fields -> aggregate-member caller_bit_fields signed_bits -> aggregate-member caller_bit_fields unsigned_bits`, recovering signed `-7` as normalized 32-bit `0xfffffff9`, unsigned `41` as `0x29`, exact bit-slice metadata, immutable core provenance, and thread/frame invalidation.
- Real `mdbg-core` exercises the same physical bit-field rendering/member-selection workflow.
- Exact implementation head `d8938b6e15bfbf7a5c5c7bce27b8f6a09bb548a1` passes full GCC / Clang-large CI plus the dedicated GCC / Clang selected-inline regression/evidence workflow.

Phase 51 is sealed here. More bit widths, alternate packing examples, extra adjacent fields, endian variants without independent compiler evidence, arbitrary bit slicing, writes/mutation, nested bit-field aggregates, and source-level ABI guesses are not new milestones.

## Phase 52 promotion — compiler-proven physical enum identity and symbolic values

The next meaningful context gap is symbolic compiler type identity. Selected-inline inspection already preserves bounded `DW_TAG_enumeration_type` metadata and exact enumerator mappings, while an ordinary historical physical frame still cannot resolve an enum root through the canonical ordinary value-type path.

The first coherent Phase 52 slice must:

1. Start from genuine GCC and Clang historical caller cores where one stack-resident enum-typed local has a compiler-produced location supported by the existing immutable frame-base machinery. Reuse the established finite enumerator shape if compiler retention is stable; do not synthesize enum DWARF or inject a value.
2. Independently prove the real `DW_TAG_enumeration_type`, exact byte width and compiler-described signed/unsigned representation, direct finite enumerator name/value table, selected lookup PC, and location/frame-base form before product assertions run.
3. Promote the already-proven selected-inline enum type resolver into the canonical root type path instead of creating a physical-only enum descriptor/parser. Preserve genuine GCC direct-encoding and Clang referenced-underlying-type spellings only as independently evidenced.
4. Materialize the physical enum through the existing immutable snapshot scalar path while preserving `LocalValueKind::Enumeration`, bounded `LocalEnumType`, normalized raw value, exact symbolic lookup, and core/runtime-artifact provenance.
5. Make selected-inline and physical enum roots share the same canonical resolver/metadata attachment boundary. Unknown numeric values must remain numeric; duplicate raw aliases must remain ambiguous rather than selecting an arbitrary symbol.
6. Prove real `mdbg-core` behavior for `frame 1 -> inline physical -> print <enum>`, including symbolic + numeric rendering and deterministic invalidation after thread/frame changes.
7. Keep enum arithmetic, mutation, flags decomposition, scoped-name reconstruction, templates/type units, recursive graphs, and expression-language semantics out of scope.
8. Require full GCC / Clang-large CI plus the relevant permanent GCC / Clang compiler/oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion continues the context-neutral typed-value architecture with a new executable historical-frame capability rather than farming more bit-field layouts.
