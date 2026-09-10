# Phase 40 Roadmap — compiler-proven bounded bit-field member semantics

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 40 extends selected-inline post-mortem aggregate inspection from byte-addressable direct members to compiler-described sub-byte integer members. Inline selection remains lexical/source ownership only; all machine state, aggregate bytes, module identity, frame/thread ownership, and provenance remain anchored in the selected immutable physical core frame and the existing snapshot-memory model.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_bit_fields` aggregate. The independent DWARF oracle proves a four-byte structure and two direct integer bit-field members before product assertions run; no hand-authored DWARF layout or source-level ABI packing inference is used.
- GCC evidence uses the compiler-emitted legacy layout form: signed five-bit `signed_bits` with `DW_AT_bit_offset = 27` and byte member offset zero, plus unsigned six-bit `unsigned_bits` with `DW_AT_bit_offset = 21` and byte member offset zero.
- Clang evidence uses the compiler-emitted modern layout form: signed five-bit `signed_bits` with `DW_AT_data_bit_offset = 0` and unsigned six-bit `unsigned_bits` with `DW_AT_data_bit_offset = 5`, without requiring a byte-level `DW_AT_data_member_location` for those fields.
- The existing bounded member descriptor is extended only with optional `LocalBitSlice { bit_offset, bit_size }` metadata. Ordinary byte-aligned members keep their existing offset/width/kind/pointer semantics unchanged.
- Selected-inline structure type recovery accepts only the compiler-proven direct integer bit-field shape, requires a non-zero bounded width no larger than the declared integer storage, requires exactly one supported bit-location spelling, and rejects malformed or aggregate-out-of-bounds slices.
- Legacy `DW_AT_bit_offset` is translated only for the project's current little-endian Linux x86-64 evidence using the compiler-described base storage width and constant member offset. `DW_AT_data_bit_offset` is consumed directly as the aggregate-relative bit location. No endian-generalization or guessed C ABI packing rule is claimed.
- Materialization extracts the described bit slice from immutable aggregate bytes, masks it to the compiler-proven width, and sign-extends signed fields to their declared integer storage width. The genuine artifact therefore yields signed five-bit `-7` as normalized 32-bit `0xfffffff9` and unsigned six-bit `41` as `0x29`.
- `CoreInspectionSession` and real `mdbg-core` reuse the existing typed aggregate/member ownership path: `caller_bit_fields` renders both fields and direct member selection preserves the same immutable `SnapshotCoreMemory` or exact owner-matched `SnapshotRuntimeArtifact` provenance. No parallel bit-field type system or mutation path is introduced.
- Existing selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread invalidation, structure/union distinction, pointer/member behavior, arrays, and older aggregate regressions remain covered by the permanent evidence suite.
- The dedicated `core-inline-evidence` gate first failed on genuine compiler differences: GCC exposed incorrect signed normalization and Clang was rejected for lacking byte-level member location. Final exact candidate `4bfcaff8c2c720a79fe044dd2be0d70403114229` then passed both GCC and Clang oracle/build/session/CLI lanes, while the normal CI matrix passed GCC and Clang-large Configure/Build/Test.
- Construction-only patch runners, scripts, and trigger files used while recovering the staged implementation are absent from the formal candidate tree.

Phase 40 is intentionally sealed here. More bit widths, another adjacent field, alternate source declarations, arbitrary bit slicing, bit-field writes, endian variants without independent compiler evidence, nested recursive layouts, or ABI guessing are not reasons to extend this phase.

## Phase 41 promotion — compiler-proven bounded enum identity and symbolic values

The next higher-value typed-value gap is semantic identity rather than another aggregate-layout variant. The selected-inline value model now represents bounded integers, pointers, structures, unions, arrays, and compiler-described bit fields, but it has no explicit representation for `DW_TAG_enumeration_type` or the compiler-provided enumerator name/value map. Treating an enum as an anonymous integer loses type-level information that a debugger can recover without inventing runtime state.

The first coherent Phase 41 slice must:

1. Start from genuine permanent-lane GCC and Clang optimized-core artifacts in which an active selected inline context retains one enum-typed local through a concrete compiler-produced location already supported by the immutable physical-frame model. If stable enum ownership is not retained by both compiler lanes, stop and record that evidence instead of manufacturing a convenient DIE or location.
2. Independently prove `DW_TAG_enumeration_type`, exact underlying byte width/signedness or compiler-described compatible base representation, and the direct `DW_TAG_enumerator` name/value entries before product assertions run.
3. Add only the bounded enum metadata required to preserve type identity and a finite compiler-proven name/value table. Existing integer semantics must remain intact; an enum must not be silently reclassified as a plain integer merely because its storage is integral.
4. Materialize exactly one compiler-proven enum value from the selected immutable snapshot frame and render both its normalized numeric value and matching symbolic enumerator when one exact mapping exists. Unknown numeric values must remain representable without guessing an enumerator.
5. Preserve selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread ownership, immutable provenance, and all existing invalidation rules. No synthetic inline machine state may be introduced.
6. Expose the proven enum through the existing typed `CoreInspectionSession` and real `mdbg-core` product path; do not introduce a general expression evaluator, enum arithmetic, mutation, flags decomposition, C++ scoped-name reconstruction, or recursive type graph as prerequisites.
7. If the genuine artifact requires unsupported dynamic locations, complex template/type-unit recovery, variant/discriminant machinery, or compiler-specific assumptions not independently evidenced in the DIEs, stop and record the blocker rather than broadening Phase 41 speculatively.

This promotion moves selected-inline post-mortem inspection from physical layout recovery to preserved compiler-level symbolic type identity while keeping Phase 40 sealed against bit-field variant farming.
