# Phase 39 Roadmap — selected-inline bounded union ownership and explicit member selection

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 39 extends selected-inline post-mortem inspection from non-overlapping scalar, structure, and fixed-array values to one compiler-owned union whose direct members share the same immutable storage. The selected inline context continues to own only lexical/DIE scope; machine state, addresses, bytes, module ownership, and provenance remain anchored in the selected physical core frame.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_union` through a concrete compiler-produced location in historical physical frame 1. No hand-authored DWARF location or synthetic inline machine state is used.
- An independent `readelf --debug-dump=info` oracle proves that the selected binding resolves to `DW_TAG_union_type`, has exact byte size 4, and contains exactly two direct compiler-described members sharing offset zero: signed four-byte `signed_value` and unsigned four-byte `unsigned_value`.
- The typed value model introduces `LocalValueKind::Union` as a distinct object kind. It does not alias union storage to `Structure`, and it does not infer an active member from overlapping bytes.
- Union type resolution unwraps only the existing bounded typedef/const chain, requires a supported direct member name and `DW_FORM_ref4` type, requires absent-or-zero `DW_AT_data_member_location`, rejects duplicate names and unsupported direct child DIEs, and keeps the existing bounded aggregate size/member-count limits.
- The first compiler-proven slice accepts only bounded integer base-type members because that is what the genuine artifact provides. No speculative pointer member, recursive object graph, discriminator recovery, variant-part interpretation, or reinterpret-cast semantics is added.
- Caller-frame union materialization reuses the existing compiler-produced frame-base path and immutable snapshot-memory reader. All direct member views decode the same shared storage at offset zero with their own compiler-proven width/signedness and preserve exact `SnapshotCoreMemory` or owner-matched `SnapshotRuntimeArtifact` provenance. Frame-zero selected-inline union materialization remains outside current evidence.
- `CoreInspectionSession::inspect_value("caller_union")` returns the bounded union container with both member choices but no claimed active member. `inspect_union_member()` performs exactly one explicit direct-member selection and rejects unknown members deterministically.
- The older structure-only `inspect_aggregate_member()` path continues to reject a union, preserving the semantic distinction between non-overlapping structure membership and explicit overlapping union views.
- Real `mdbg-core` exposes a distinct `union-member <name> <member>` command. The genuine-core workflow proves `frame 1 -> selected caller_inline_inner -> print caller_union -> union-member caller_union signed_value -> union-member caller_union unsigned_value`, plus deterministic rejection of a missing member.
- The permanent `core-inline-evidence` workflow verifies genuine GCC and Clang PIE/non-PIE compiler artifacts and the independent union oracle before building and running the existing aggregate/array regressions plus the new union session/CLI integration. Exact candidate `96431019ae040d16c51af76af18d7470ea2fbff8` passed both compiler lanes of that dedicated evidence gate and the normal GCC/Clang-large Configure/Build/Test matrix.
- Construction-only repair machinery used while recovering the staged implementation is absent from the formal candidate; the permanent workflow contains only executable compiler/oracle/session evidence.

Phase 39 is intentionally sealed here. More union members, alternate integer widths, another member spelling, guessed active-member rules, pointer-member variants without compiler evidence, or recursive/nested union traversal are not reasons to extend this phase.

## Phase 40 promotion — compiler-proven bounded bit-field member semantics

The next higher-value typed-layout gap is non-byte-aligned member storage. The current structure/union descriptors can represent direct members only as byte offset plus byte width. They do not model compiler-described bit-field location/width metadata, so a genuine C/C++ bit-field cannot be exposed without either losing layout information or guessing ABI packing rules.

The first coherent Phase 40 slice must:

1. Start from genuine permanent-lane GCC and Clang optimized-core artifacts in which a selected inline context owns a bounded aggregate containing at least one retained bit-field member. If the compilers do not preserve a stable binding and explicit bit-field metadata within the current immutable physical-frame model, stop and record that evidence instead of manufacturing a layout.
2. Independently prove the compiler-emitted aggregate type, member identity, base type, storage size, and exact DWARF bit-location/bit-width attributes before product assertions run. The debugger must not infer GCC/Clang ABI bit packing from C source declarations.
3. Extend the existing bounded member descriptor only as far as required to distinguish byte-aligned members from compiler-described bit slices. Existing ordinary structure/union members must retain their current semantics and provenance.
4. Extract exactly one compiler-proven bit-field value from immutable snapshot bytes with checked bounds and correct signed/unsigned normalization. No bit-field writes, mutation, arbitrary bit slicing, endian-generalization claim, or recursive object graph belongs in the first slice.
5. Preserve selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread ownership and all invalidation rules before any bit slice is exposed. Every storage byte must still come from the selected immutable physical core frame or exact runtime artifact.
6. Expose the proven member through the existing typed session/product ownership model, using a distinct semantic path only where the current byte-aligned member contract cannot express the compiler evidence; do not build a parallel type system.
7. If satisfying the artifact requires unsupported dynamic member locations, DWARF expressions unrelated to the bit-field itself, inheritance, variant parts, recursive/nested object graphs, or guessed compiler ABI behavior, stop and record the blocker rather than broadening the phase speculatively.

This promotion moves the selected-inline object model from overlapping byte-addressable storage to compiler-described sub-byte layout while keeping Phase 39 sealed against union variant farming.
