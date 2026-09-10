# Phase 38 Roadmap — selected-inline bounded fixed-array values and indexing

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 38 extends selected-inline post-mortem inspection with one genuinely different typed container: a compiler-owned fixed-size array local. Inline selection remains lexical/source ownership only; array storage, frame state, addresses, bytes, module ownership, and provenance remain owned by the selected immutable physical core frame and the existing snapshot-memory model.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_fixed_array`. The independent DWARF oracle proves a real `DW_TAG_array_type`, one direct subrange, signed four-byte integer element type, and element count 3 before product assertions run. No hand-authored array DIE, guessed count, or convenience-only location expression is used.
- The bounded type model adds `LocalValueKind::Array` plus exact element-count, element-width, signedness, and element-kind metadata without replacing the existing scalar/pointer/structure representation with a recursive generic object graph.
- Array type resolution unwraps only the already-bounded typedef/const chain, requires one compiler-described direct subrange, accepts bounded compiler scalar/count forms, requires zero/absent lower bound, rejects inconsistent count/upper-bound metadata, and caps element count at 64, element width at eight bytes, and total storage at the existing bounded local-object size.
- Historical selected-inline array materialization uses the existing frame-base/address and immutable snapshot-memory machinery. The resulting three elements are exactly `0x10203040`, `0x22334455`, and `0x33445566`, and storage provenance remains `SnapshotCoreMemory` or the exact owner-matched `SnapshotRuntimeArtifact`.
- `CoreInspectionSession::inspect_array_element()` permits exactly one checked explicit integer index. Index 1 returns the signed four-byte integer `0x22334455` with preserved snapshot provenance; index 3 is rejected deterministically as out of range. No slices, pointer arithmetic, decay, multidimensional arrays, VLAs, or recursive element traversal are introduced.
- Real `mdbg-core` renders `caller_fixed_array = [0x10203040, 0x22334455, 0x33445566]` and exposes `array-element <name> <index>` through the same selected-frame/selected-inline ownership rules as the session API. Invalid frame/thread/inline ownership continues to invalidate the operation rather than manufacturing an inline machine frame.
- Existing selected-inline scalar, pointer, direct-structure, aggregate-member, pointer-member, dereference, lexical-shadowing, module/thread/frame validation, and immutable-provenance behavior remains independently covered by the permanent suite.
- Test-first exact head `646171b945fe05ba02d64d98ac1ea9ce6841cc02` passed the genuine GCC/Clang array DWARF oracle but failed while building the core inspection surface because Array representation/session indexing did not yet exist. Implementation commit `1cdb2e52b400cc82a0b744f50905ee3ba4db29ad` supplies the bounded production model. Implementation-equivalent verification head `f7a5368cbf7e9c2460907d8d3060628234621c4b` then passed both the normal GCC/Clang-large suite and the dedicated GCC/Clang compiler/core/session/CLI evidence gate; its only extra tree entry was a temporary CI trigger removed before the formal candidate.

Phase 38 is intentionally sealed here. More array lengths, alternate integer widths, a second subrange spelling, multidimensional arrays, VLAs, array decay, pointer arithmetic, nested arrays, or general expression syntax are not reasons to extend this phase.

## Phase 39 promotion — selected-inline bounded union ownership and explicit member selection

The next higher-level typed-object gap is overlapping storage rather than another sequential-container variant. The selected-inline model can now materialize scalar, pointer, structure, and fixed-array locals, but it has no representation or navigation rule for a compiler-owned union whose members share storage and for which the debugger cannot safely infer an active member.

The first coherent Phase 39 slice is evidence-gated and must:

1. Start with genuine permanent-lane GCC and Clang optimized-core artifacts in which the selected inline DIE owns one union local through a concrete compiler-produced location. If both compilers do not retain stable union ownership inside the current immutable physical-frame model, stop and record the evidence instead of manufacturing a union.
2. Prove the real `DW_TAG_union_type`, exact byte size, and direct member DIE/type metadata independently before product assertions run. Do not add hand-authored DWARF or infer a discriminator that the compiler did not describe.
3. Preserve overlapping-storage semantics explicitly. The debugger may expose the union as a bounded typed container and allow one user-selected direct member, but it must not claim to know which member is active merely from the shared bytes.
4. Reuse existing bounded scalar/pointer member descriptors and immutable snapshot-memory/provenance machinery where their invariants fit. Do not introduce a recursive generic object graph, arbitrary reinterpret-casts, nested aggregate traversal, or a new expression language as a prerequisite.
5. Permit exactly one explicit direct-member selection with deterministic unknown-member/type/layout rejection. Any pointer-valued member may only inherit an already-supported bounded pointee descriptor; deeper dereference or nested traversal requires separate evidence and acceptance.
6. Expose the proven behavior through both `CoreInspectionSession` and a distinct real `mdbg-core` product command while preserving selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread ownership and invalidation.
7. If the compiler artifact requires dynamic discriminant recovery, variant-part DWARF, recursive type graphs, unsupported location expressions, or guessed active-member semantics, stop and record that blocker instead of weakening the ownership model.

This promotion moves the typed post-mortem object model from non-overlapping containers to explicit overlapping-storage semantics while keeping Phase 38 sealed against array-shape farming.
