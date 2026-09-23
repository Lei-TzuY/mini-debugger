# Phase 77 Roadmap — compiler-proven current-frame live union ownership and explicit member selection

Status: complete for the current Linux x86-64 optimized live-debugger milestone.

Phase 77 closes the remaining canonical aggregate-root gap in current-frame live inspection. A genuine optimized union local can now be recovered from compiler-produced stack ownership, materialized through the shared bounded union byte model, rendered without inferring an active member, and inspected through the existing context-neutral explicit union-member selector.

## Completed acceptance

- The permanent optimized live fixture retains a genuine four-byte `union LiveUnion` with overlapping `int32_t signed_value` and `uint32_t unsigned_value`, initialized to `0x44556677`, at exact exported stop `live_union_probe`.
- Independent PIE/non-PIE readelf oracles prove before product assertions:
  - real `DW_TAG_union_type`;
  - exact four-byte extent;
  - exactly two direct offset-zero/absent-offset members;
  - signed and unsigned four-byte compiler base types;
  - lexical ownership in `inspect_live_union`;
  - active compiler-produced `DW_OP_fbreg` ownership at the exact probe.
- Test-first head `a800250b90315dbf35b3683ea569bb7946d41b65` keeps the established suite intact, passes both live-union compiler oracles, and fails only at the explicit product boundary `live union materialization is outside current compiler evidence`.
- Production head `9d728d92254a924a79f9fd9153877f897a2f2d05` introduces no new union parser, type descriptor, location evaluator, or member selector:
  - the existing canonical union byte materializer gains an ownership-neutral module-path overload while the snapshot-owner overload delegates to it;
  - current-frame live lookup accepts only the compiler-proven `DW_OP_fbreg` union path, reads exactly the bounded byte extent from the stopped tracee, and delegates to the shared union materializer;
  - scalar register/entry/breg evaluators remain unavailable to unions and unsupported non-fbreg union ownership remains explicitly rejected;
  - the live CLI renders `union{signed_value, unsigned_value}` without choosing an active member and exposes `union-member <name> <member>` through the existing `inspect_local_union_member()`.
- API evidence proves both overlapping signed/unsigned views recover exactly `0x44556677`, preserve compiler-described width/signedness, and reject a missing member deterministically.
- Real `mdbg` subprocess evidence proves `break live_union_probe -> continue -> print live_union -> union-member live_union signed_value -> union-member live_union unsigned_value -> union-member live_union missing`.
- Initial production validation exposed only a test-authored diagnostic mismatch: the existing shared context-neutral selector still carries the historical exact error text `bounded selected-inline union has no member named: missing`, which older permanent tests explicitly assert. Head `cdb741ba51716d12ecd53542cc8a0c436cc2bd1e` corrects the new test to that exact established diagnostic without weakening or changing production behavior.
- Exact candidate `cdb741ba51716d12ecd53542cc8a0c436cc2bd1e` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler/core/session/CLI evidence lanes, based directly on main `a6936b73b54a4271d0553a901469048d68348947`, ahead three / behind zero.

Phase 77 is sealed here. Additional union member widths, names, fbreg offsets, active-member inference, discriminators/variant parts, nested unions, pointer-member variants, reinterpretation, mutation, and unsupported location forms are not new milestones.

## Phase 78 promotion — compiler-proven current-frame live bit-field structure semantics

The next executable current-frame live gap is compiler-described sub-byte structure layout, not another union variant. Canonical bit-slice type recovery and checked signed/unsigned extraction already exist across immutable physical and selected-inline ownership, while the live structure decoder still explicitly rejects canonical `LocalBitSlice` metadata with `live bit-field structure materialization is outside current compiler evidence`.

The first coherent Phase 78 slice must:

1. Start from a genuine optimized GCC and Clang current-frame fixture containing one small stack-owned structure with retained signed and unsigned integer bit fields active at an exact debugger stop.
2. Independently prove the real structure DIE, exact byte extent, member names/base types, exact compiler-emitted bit widths and bit-location spellings, lexical ownership, exact stop PC, and active compiler-produced location before product assertions run. Do not infer packing from C source.
3. Accept only current-frame location/frame-base forms actually emitted by the permanent compiler lanes. Reuse existing current-frame frame-base/address machinery rather than adding a general DWARF-expression VM.
4. Reuse canonical `resolve_bounded_bit_field_member_type()`, `LocalBitSlice`, and `decode_bounded_bit_field()`; no live-only bit-field descriptor/parser/decoder is allowed.
5. Extend live structure byte materialization only enough to consume those canonical slices from stopped-tracee bytes, preserving signed normalization and exact bounded layout checks.
6. Reuse the existing structure renderer and context-neutral aggregate-member selector. Prove both the aggregate rendering and explicit signed/unsigned member views through the real `mdbg` CLI.
7. Preserve stopped-tracee freshness and deterministic malformed/out-of-range metadata failures. Keep bit-field writes, arbitrary bit slicing, endian/ABI widening, nested bit-field aggregates, mutation, and unsupported location forms out of scope.
8. Require full GCC / Clang-large CI plus permanent compiler-oracle/API/CLI evidence on the exact candidate before integration.

This promotion adds a new live typed-object capability rather than farming more union cases.
