# Phase 79 Roadmap — compiler-proven current-frame live enum-valued structure members

Status: complete for the current Linux x86-64 optimized live-debugger milestone.

Phase 79 closes the remaining current-frame live direct-member type gap exposed by Phase 78. Canonical enum typing, finite enumerator identity, generic structure bytes, and context-neutral aggregate-member selection already existed; this phase proves that a genuine stopped live structure can carry one enum-valued direct member without a live-only enum engine.

## Completed acceptance

- The permanent optimized live fixture now retains one genuine eight-byte `LiveEnumAggregate live_enum_aggregate` in `inspect_live_enum_aggregate`, with signed `direct = 0x31415926` at offset 0 and `LiveMode mode = LiveBusy` at offset 4.
- An independent readelf oracle proves under both GCC and Clang, PIE and non-PIE:
  - the outer value is a real eight-byte `DW_TAG_structure_type`;
  - `direct` is a compiler-described signed 32-bit integer at offset 0;
  - `mode` is a four-byte unsigned `DW_TAG_enumeration_type` at offset 4;
  - the enum is exactly `LiveMode` with `LiveIdle = 3`, `LiveReady = 7`, and `LiveBusy = 42`;
  - ownership at `live_enum_aggregate_probe` is the already-supported compiler-produced `DW_OP_fbreg` form.
- Exact test-first head `898d4266baa705f745abd03149db5e08de9f7786` leaves production unchanged. Both new PIE/non-PIE enum-aggregate oracles pass under GCC and Clang, the established suite remains intact, and both normal compiler lanes fail only at the intended product boundary: `live enum-valued structure materialization is outside current compiler evidence`.
- Production head `9f2bf426b73f0e8e2078cb527810704b69847644` removes only that four-line guard. It adds no parser, descriptor, location evaluator, materializer, enum lookup implementation, or traversal primitive.
- `decode_structure()` reuses the existing checked byte decode, `LocalValueKind::Enumeration`, and `LocalStructMemberType::enum_type`; exact raw value and canonical `LocalEnumType` metadata flow into the materialized member unchanged.
- `inspect_local_aggregate_member()` is reused unchanged. Selecting `live_enum_aggregate.mode` returns raw value 42 plus the exact `LiveMode` metadata, and `local_enum_symbol()` resolves it conservatively to `LiveBusy`.
- Permanent API evidence also proves unknown raw value 11 remains numeric and duplicate raw aliases remain symbolically ambiguous instead of selecting an arbitrary name.
- Live `print` renders the whole structure with raw direct-member values, while Phase-78 `aggregate-member live_enum_aggregate mode` renders `LiveMode::LiveBusy (0x2a)` through the existing symbolic enum path.
- Real workflow evidence proves `break live_enum_aggregate_probe -> continue -> print live_enum_aggregate -> aggregate-member live_enum_aggregate mode -> continue`, exact values, and clean process completion.
- Exact production head `9f2bf426b73f0e8e2078cb527810704b69847644` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler-evidence lanes, based directly on main `7aec4aa252168f764f5315cb935a1b4fc8fdce56`.

Phase 79 is sealed here. More enum values, aliases, signed enum variants, additional enum-valued fields, arrays/unions of enums, writes, flags decomposition, casts, arithmetic, and cosmetic rendering variants are not new milestones.

## Phase 80 promotion — compiler-proven current-frame live pointer-valued aggregate member dereference

The next meaningful executable gap is pointer-member traversal in the current physical frame. Canonical direct pointer-member typing, live structure byte materialization, explicit aggregate-member selection, and the bounded stopped-tracee pointer dereference engine already exist and are proven independently. The live CLI, however, currently stops at `aggregate-member`; the core CLI already has `deref-aggregate-member`, but the live debugger surface does not yet expose the equivalent fixed-depth operation.

The first coherent Phase 80 slice must:

1. Start from a genuine optimized GCC/Clang current-frame live structure retained at a compiler-produced location already supported by the current-frame frame-base machinery. The structure must contain:
   - one ordinary scalar direct member; and
   - one pointer-valued direct member whose pointee is exactly one bounded signed 32-bit integer.
2. Independently prove the outer structure DIE/extent, member names/offsets/types, pointer width, signed-int32 pointee metadata, lexical ownership, exact stop PC, active compiler location form, runtime pointer address, and pointee value before product assertions run.
3. Reuse `resolve_bounded_direct_structure_member_type()`, canonical pointer/pointee metadata, `decode_structure()`, and `inspect_local_aggregate_member()`; do not add a live-only pointer-member descriptor/parser.
4. Reuse the existing `dereference_local_pointer(Debugger, InspectionFrameContext, LocalScalarValue)` stopped-tracee read for exactly one dereference. No second memory reader or pointer traversal engine is allowed.
5. Expose a live `deref-aggregate-member <name> <member>` command by composing existing root inspection, aggregate-member selection, current-frame construction/validation, and the existing one-hop dereference primitive.
6. Preserve deterministic failures for non-pointer members, null pointers, malformed/missing pointee metadata, unreadable/short memory, and stale frame identity.
7. Keep pointer chains, pointer arithmetic, casts, implicit dereference, mutation, arbitrary dotted paths, recursive graphs, structure pointees, and unsupported ownership forms out of scope for this slice.
8. Require full GCC / Clang-large CI plus independent compiler oracle/API/CLI evidence on the exact candidate before integration.

This promotion moves from typed-member identity to fixed-depth live object reachability rather than farming additional enum-member variants.
