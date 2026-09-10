# Phase 37 Roadmap — selected-inline direct aggregate member traversal

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 37 extends the Phase 36 selected-inline by-value aggregate from whole-value materialization to one typed direct-member selection and one bounded dereference of a pointer-valued direct member. The selected inline context remains source/DIE ownership only; every byte and address still belongs to the immutable selected physical core frame and existing snapshot-memory model.

## Completed acceptance

- The permanent GCC and Clang optimized-core lanes reuse the existing genuine `caller_direct_aggregate` owned by selected `caller_inline_inner`; no new structure, hand-authored DWARF expression, or compiler convenience variant is introduced for this phase.
- Phase 36 continues to materialize `caller_direct_aggregate` as the existing bounded `LocalValueKind::Structure`, with exactly the compiler-proven `direct` integer member and `linked` pointer member plus immutable core/runtime-artifact provenance.
- Direct aggregate traversal consumes those already-materialized `LocalScalarValue::members`. It does not reparse the aggregate DIE, recompute member layout, synthesize an inline machine frame, or reread aggregate storage merely to select a member.
- `CoreInspectionSession::inspect_aggregate_member()` is deliberately distinct from the older pointer-root `inspect_pointer_member()` path. It requires an active selected-inline owner, reuses normal selected frame/module/thread validation, and rejects physical/no-inline use rather than silently broadening semantics.
- `caller_direct_aggregate.direct` materializes exactly as the signed four-byte integer `0x55667788` and retains the aggregate's immutable snapshot provenance.
- `caller_direct_aggregate.linked` materializes exactly as an x86-64 pointer with the already-proven bounded signed-int pointee descriptor. Pointer metadata is copied from the materialized member rather than reconstructed from a second type system.
- `CoreInspectionSession::dereference_aggregate_member()` permits exactly one further dereference for that pointer-valued direct member. It preserves null/host-width/unreadable/provenance failures and reads the genuine immutable pointee `caller_inline_pointee == 0x02468ace` through the existing snapshot-memory resolver.
- Empty/unknown member names, unsupported member kinds, malformed scalar widths, malformed pointer metadata, non-structure roots, absent selected-inline ownership, and unreadable pointees remain explicit failures.
- Existing pointer-to-structure member traversal is unchanged and remains independently exercised in the same product regression: `caller_aggregate_pointer->direct == 0x11223344` and `*(caller_aggregate_pointer->linked) == 0x13579bdf`.
- Real `mdbg-core` exposes a separate command surface: `aggregate-member <name> <member>` and `deref-aggregate-member <name> <member>`. The older `member` / `deref-member` commands retain pointer-root semantics.
- The genuine-core product workflow proves `frame 1 -> inline caller_inline_inner -> print caller_direct_aggregate -> aggregate-member caller_direct_aggregate direct -> deref-aggregate-member caller_direct_aggregate linked`, then still exercises inline/frame/thread invalidation and the legacy pointer-root commands.
- Test-first exact head `da9da4a5f05c48289bd9f8e3305a4b4ccf86ee19` passed compiler fixture/oracle evidence and built `mdbg-core`, then failed exactly because `CoreInspectionSession` had no `inspect_aggregate_member()` / `dereference_aggregate_member()` API. Exact production head `66d735edace0010474d9d8e9e90221f722a43370` passes the dedicated GCC/Clang genuine-core product gate and the normal permanent GCC/Clang-large Configure/Build/Test matrix.

Phase 37 is intentionally sealed here. More structure members, alternate member widths, another `DW_OP_fbreg` offset, nested structures, pointer chains, arrays, unions, inheritance, and a general expression language are not reasons to extend this phase.

## Phase 38 promotion — selected-inline bounded fixed-array values and indexing

The next higher-level typed-value gap is a new object form rather than another structure-member variant. Selected-inline inspection can now materialize bounded scalar, pointer, by-value structure, pointer-to-structure member, and direct-structure member values, but it has no typed representation or bounded navigation rule for a compiler-owned fixed-size array local.

The first coherent Phase 38 slice is evidence-gated and must:

1. Start with genuine permanent-lane GCC and Clang optimized-core artifacts in which the selected inline DIE owns one fixed-size array local through a concrete compiler-produced location. If both compilers do not retain a stable binding within the current immutable physical-frame model, stop and record that evidence instead of manufacturing an array.
2. Prove the real `DW_TAG_array_type`, bounded subrange/count, element type, and active variable ownership independently before product assertions run. No hand-authored DWARF array metadata or forced location expression is allowed.
3. Keep all machine-state ownership physical: inline selection remains lexical/source scope only, while array bytes, frame base/CFA, addresses, module ownership, and provenance come from the selected immutable physical snapshot frame and existing snapshot-memory machinery.
4. Introduce one bounded fixed-array representation or reuse an existing typed-value form only if it can preserve exact element count/type/width without weakening the current structure/pointer invariants. Do not build a recursive generic object graph as a prerequisite.
5. Permit exactly one checked element selection by explicit integer index, with deterministic bounds rejection and immutable provenance. No slices, pointer arithmetic, multidimensional arrays, variable-length arrays, array decay, recursive element traversal, or expression grammar in the first slice.
6. Expose the proven behavior through both `CoreInspectionSession` and a distinct real `mdbg-core` command/API surface, while preserving selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread ownership and invalidation.
7. If supporting the compiler artifact requires general DWARF-expression widening, recursive type graphs, or guessed bounds, stop and record the blocker rather than weakening the evidence discipline.

This promotion moves post-mortem selected-inline inspection to a genuinely different typed container semantics while keeping Phase 37 sealed against member-layout variant farming.
