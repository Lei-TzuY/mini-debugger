# Phase 42 Roadmap — compiler-proven enum identity through aggregate members

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 42 composes the symbolic enum identity established in Phase 41 through an already-supported aggregate-member boundary. Selected-inline ownership remains lexical/source context only; the selected physical core frame continues to own bytes, register state, module identity, frame/thread identity, and immutable snapshot provenance.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_typed_aggregate` through a compiler-produced caller-frame `DW_OP_fbreg` location. No synthetic DIE, hand-authored location expression, or product-side value injection is used.
- The independent DWARF oracle proves the selected inline chain `caller_typed_inline_inner -> caller_typed_inline_outer -> caller_typed_physical_frame`, an eight-byte compiler-owned `CallerInlineTypedAggregate`, direct members `direct@0` and `mode@4`, and exact direct-member type ownership before product assertions run.
- The ordinary `direct` member remains a four-byte signed integer, while `mode` is independently proven as the four-byte unsigned `CallerInlineTypedMode` `DW_TAG_enumeration_type` with direct compiler enumerators `CallerInlineTypedIdle = 3`, `CallerInlineTypedReady = 7`, and `CallerInlineTypedBusy = 42`.
- The existing bounded member descriptor is extended only with optional enum metadata. Integer members reject pointer/enum metadata, pointer members reject enum metadata, and enum members require representation-consistent non-empty bounded enum metadata rather than being silently reclassified as integers.
- Aggregate materialization carries enum identity from the compiler-described member type through `LocalStructMemberType` and immutable `LocalStructMember` materialization. Selecting the direct `mode` member through the existing aggregate-member path yields `LocalValueKind::Enumeration`, the exact enum metadata, normalized raw value `42`, and symbolic `CallerInlineTypedBusy`.
- The selected enum leaf preserves `SnapshotCoreMemory` or exact owner-matched `SnapshotRuntimeArtifact` provenance from the aggregate bytes. No synthetic inline register or machine frame is created.
- Exact symbolic lookup remains conservative: an unknown raw value remains numeric, while duplicate raw-value aliases are treated as ambiguous instead of selecting an arbitrary enumerator.
- Real `mdbg-core` proves the product path with `frame 1 -> inline caller_typed_inline_inner -> print caller_typed_aggregate -> aggregate-member caller_typed_aggregate mode`, rendering `CallerInlineTypedMode::CallerInlineTypedBusy (0x2a)` together with `[4-byte enum unsigned]`.
- Existing aggregate bounds, integer/pointer/member behavior, arrays, unions, compiler-described bit fields, direct enum locals, lexical/abstract-origin ownership, frame/thread/module invalidation, and immutable provenance remain covered by the permanent suites.
- After the construction-only Phase 42 patch workflow had produced the bounded implementation, it was removed from the candidate tree. Exact clean head `325b21287f3c9ad14bcd54a336c991ec5535ef09` passed both the normal GCC/Clang-large Configure/Build/Test matrix and the dedicated GCC/Clang `core-inline-evidence` compiler/oracle/core/session/CLI gate.

Phase 42 is intentionally sealed here. More enum-valued members, alternate enumerator counts, signed enum variants, another member offset, typedef spelling variants, arrays of enums, or additional scalar container combinations are not reasons to extend this phase without a new ownership capability and independent compiler evidence.

## Phase 43 promotion — compiler-proven bounded nested aggregate composition

The next higher-value type-composition gap is structural depth rather than another scalar leaf. The selected-inline model can materialize bounded top-level structures/unions/arrays and can now preserve integer, pointer, bit-field, and enum semantics through one direct structure-member boundary, but a structure-valued direct member still has no bounded descriptor/materialization/navigation contract. Treating an inner aggregate as an integer or flattening its bytes would lose compiler-owned structure identity and member ownership.

The first coherent Phase 43 slice must:

1. Start from genuine permanent-lane GCC and Clang optimized-core artifacts in which an active selected inline context retains one bounded outer structure containing one direct inner structure through compiler-produced locations already supported by the immutable snapshot model. If both compilers do not retain a stable outer/inner/member layout, stop and record the evidence instead of manufacturing one.
2. Independently prove the outer and inner `DW_TAG_structure_type` DIEs, exact byte sizes, direct member ownership and constant offsets, and the terminal scalar member type before product assertions run.
3. Extend the existing typed member descriptor only enough to represent one compiler-proven structure-valued direct member with a finite bounded child-member set. Reuse the current member invariants and immutable snapshot-memory machinery rather than introducing a second aggregate model.
4. Materialize the inner aggregate strictly from the byte range owned by the outer aggregate and preserve the original `SnapshotCoreMemory` or exact owner-matched `SnapshotRuntimeArtifact` provenance. Nested selection must not synthesize a new machine frame or perform an unrelated memory search.
5. Provide exactly one additional explicit member-selection hop through `CoreInspectionSession` and real `mdbg-core`, sufficient to demonstrate `outer -> inner -> terminal member`. The API may use a bounded explicit two-hop operation, but it must not introduce arbitrary recursive path parsing or a general expression language.
6. Preserve the existing integer/pointer/enum/bit-field direct-member semantics, array/union behavior, symbolic enum ambiguity rules, lexical/inline ownership, frame/thread/module invalidation, aggregate size/member-count limits, and artifact ownership.
7. Reject deeper recursive aggregates, cycles/self-reference, dynamic member locations, inheritance, discriminated variants, flexible arrays, compiler-specific ABI guesses, and any layout not independently represented in the genuine DWARF evidence. If the first genuine compiler artifact requires those features, promote the blocker rather than widening Phase 43 speculatively.

This promotion tests whether the bounded typed-object model can preserve compiler-owned structure identity across one genuinely deeper ownership boundary instead of farming more terminal scalar variants.
