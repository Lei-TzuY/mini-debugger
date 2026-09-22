# Phase 50 Roadmap — compiler-proven physical union ownership and explicit member selection

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 50 closes the union asymmetry between selected-inline and historical physical-frame inspection. A historical physical caller can now own and materialize one bounded overlapping-storage union through the canonical root type path and expose explicit member views through the same immutable snapshot/provenance rules already used by selected-inline unions.

## Completed acceptance

- The permanent historical caller fixture now retains a genuine stack-resident `caller_union` with one signed 32-bit `signed_value` and one unsigned 32-bit `unsigned_value` sharing the same four bytes initialized to `0x44556677`.
- An independent readelf oracle proves GCC and Clang both retain the historical variable through compiler-produced `DW_OP_fbreg` ownership, a real `DW_TAG_union_type`, exact four-byte union extent, exactly two direct members, absent-or-zero overlapping member offsets, signed/unsigned four-byte base types, the caller lookup PC, and the existing frame-base form before product assertions run.
- Test-first exact head `f3fd8bac5178e17b85b195a42b2b383aff88d4a1` kept the existing 54-test suite green under both normal compiler lanes and reached the genuine core, then failed under both GCC and Clang at the intended architecture boundary: ordinary `resolve_value_type()` did not recognize a union root.
- One canonical `resolve_bounded_union_type()` and `resolve_bounded_union_member_type()` now serve ordinary physical and selected-inline root resolution. The selected-inline-only union tag, resolver, and member resolver are removed.
- The canonical union resolver accepts only the compiler-proven bounded integer member shape, requires absent-or-zero overlapping offsets, rejects duplicate names and unsupported child/member types, and preserves the existing aggregate byte/member limits. It does not infer an active member.
- `materialize_snapshot_memory_value()` now materializes bounded unions from the same root-owned immutable bytes. Every explicit member view decodes offset zero using its own compiler-described width/signedness and preserves exact `SnapshotCoreMemory` or owner-matched `SnapshotRuntimeArtifact` provenance. The selected-inline-only union materializer is removed.
- Live ptrace union materialization remains explicitly outside current compiler evidence; sharing the root type descriptor does not silently broaden mutable live-process semantics. Unsupported snapshot register/`DW_OP_addr` union forms remain rejected.
- One context-neutral `inspect_local_union_member()` performs explicit bounded member selection. `CoreInspectionSession::inspect_union_member()` now resolves physical or selected-inline source ownership first and then invokes that shared selector; the existing selected-inline wrapper remains only as a compatibility forwarding surface.
- Real API evidence proves `frame 1 -> inline physical -> print caller_union -> union-member caller_union signed_value -> union-member caller_union unsigned_value`, exact raw-value recovery, deterministic missing-member rejection, structure-selector rejection, immutable provenance, and invalidation across thread/frame changes.
- Real `mdbg-core` exercises the same physical union rendering and explicit member-selection workflow without claiming an active member.
- Exact implementation head `c70d2bfb294aa7f76845ff527e932a3685179ea6` passes full GCC / Clang-large CI plus the dedicated GCC / Clang selected-inline regression/evidence workflow.

Phase 50 is sealed here. More union members, alternate scalar widths, alternate overlapping declarations, guessed active-member rules, pointer-member variants without new evidence, discriminators, variant parts, nested unions, mutation, and arbitrary reinterpretation are not new milestones.

## Phase 51 promotion — compiler-proven physical bit-field member semantics

The next meaningful context gap is compiler-described sub-byte layout. Selected-inline inspection already preserves GCC/Clang bit-field metadata and extracts bounded signed/unsigned bit slices, while an ordinary historical physical structure still routes direct members through byte-aligned member resolution and therefore cannot describe the same compiler-owned bit fields.

The first coherent Phase 51 slice must:

1. Start from genuine GCC and Clang historical caller cores where one stack-resident structure contains retained signed and unsigned bit fields and has a compiler-produced location supported by the existing immutable frame-base machinery. Do not synthesize DWARF or infer packing from the C declaration.
2. Independently prove the physical structure DIE, exact byte extent, direct member identities/base types, each compiler-emitted bit width/location spelling, selected lookup PC, and location/frame-base form before product assertions run.
3. Promote the already-proven selected-inline bit-slice resolver into the canonical structure-member type path rather than adding a physical-only bit-field parser. Preserve GCC legacy `DW_AT_bit_offset` and Clang `DW_AT_data_bit_offset` handling only to the extent already evidenced on little-endian Linux x86-64.
4. Make generic immutable structure materialization honor canonical `LocalBitSlice` metadata using one shared checked extraction/sign-normalization primitive. Physical and selected-inline structures must consume the same compiler-described slice semantics.
5. Expose physical bit-field values through the existing structure renderer and context-neutral aggregate-member selector; preserve member provenance and reject malformed/out-of-bounds slices deterministically.
6. Prove real `mdbg-core` behavior for `frame 1 -> inline physical -> print <bit-structure> -> aggregate-member <bit-structure> <signed-field> -> aggregate-member <bit-structure> <unsigned-field>`, plus thread/frame invalidation.
7. Keep bit-field writes, arbitrary bit slicing, new endian/ABI claims, nested bit-field aggregates, recursive graphs, mutation, and expression-language semantics out of scope.
8. Require full GCC / Clang-large CI plus the relevant permanent GCC / Clang compiler/oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion continues outward with compiler-evidenced historical-frame capability rather than farming union variants.
