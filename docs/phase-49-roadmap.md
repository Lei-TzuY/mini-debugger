# Phase 49 Roadmap — compiler-proven physical fixed-array ownership and indexing

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 49 closes the fixed-array asymmetry between selected-inline and historical physical-frame inspection. A historical physical caller can now own and materialize one bounded one-dimensional scalar array through the canonical root type path and expose checked indexing through the same immutable snapshot/provenance rules already used by selected-inline arrays.

## Completed acceptance

- The permanent historical caller fixture now retains a genuine stack-resident `int32_t caller_fixed_array[3]` with values `0x10203040`, `0x22334455`, and `0x33445566`.
- An independent readelf oracle proves GCC and Clang both retain the historical variable through compiler-produced `DW_OP_fbreg` ownership, a real `DW_TAG_array_type`, exactly one direct subrange, zero/absent lower bound, element count 3, signed four-byte integer element type, twelve-byte total extent, the caller lookup PC, and the existing frame-base form before product assertions run.
- Test-first exact head `07dbc2e11e7fc915537ba5a06005c0a453103b9b` kept the existing 54-test suite green under both normal compiler lanes and reached the genuine core, then failed under both GCC and Clang at the intended architecture boundary: ordinary `resolve_value_type()` did not recognize an array root.
- `LocalValueType` now carries optional bounded `LocalArrayType` metadata, preserving exact element count, width, signedness, element kind, and total root byte extent without introducing a recursive generic object graph.
- One canonical `resolve_bounded_fixed_array_type()` now serves ordinary physical and selected-inline root resolution. The selected-inline-only fixed-array descriptor/parser is removed.
- `materialize_snapshot_memory_value()` now materializes bounded arrays directly from bytes already owned by the root value and preserves exact `SnapshotCoreMemory` or owner-matched `SnapshotRuntimeArtifact` provenance. The selected-inline-only array materializer is removed.
- Live ptrace array materialization remains explicitly outside current compiler evidence; promoting the shared type descriptor does not silently broaden mutable live-process semantics.
- One context-neutral `inspect_local_array_element()` performs checked scalar indexing and provenance offset composition. `CoreInspectionSession::inspect_array_element()` now resolves physical or selected-inline source ownership first and then invokes that shared primitive.
- Real API evidence proves `frame 1 -> inline physical -> print caller_fixed_array -> array-element caller_fixed_array 1`, exact recovery of `0x22334455`, deterministic index-3 rejection, immutable core provenance, and invalidation across thread/frame changes.
- Real `mdbg-core` exercises the same physical array rendering and indexing workflow and reports the same out-of-range failure.
- Exact implementation head `13440873f2fe07703d321abc482be60eae06ab4c` passes full GCC / Clang-large CI plus the dedicated GCC / Clang selected-inline regression/evidence workflow.

Phase 49 is sealed here. More array lengths, alternate scalar widths, additional bound spellings, multidimensional arrays, VLAs, slices, pointer arithmetic, decay-to-pointer behavior, mutation, or nested arrays are not new milestones.

## Phase 50 promotion — compiler-proven physical union ownership and explicit member selection

The next meaningful context gap is overlapping storage. Selected-inline inspection already owns a bounded compiler-proven union and explicit member selection, while an ordinary historical physical frame still cannot describe or materialize a union root or expose an explicit overlapping member view.

The first coherent Phase 50 slice must:

1. Start from a genuine GCC and Clang historical caller core where one stack-resident union has a compiler-produced location supported by the existing immutable frame-base machinery. Do not synthesize union DWARF or force a convenience-only location expression.
2. Independently prove the real `DW_TAG_union_type`, exact byte size, direct member names/types, absent-or-zero overlapping offsets, selected lookup PC, and location/frame-base form before product assertions run.
3. Promote the already-bounded selected-inline union type resolver into the canonical root type path rather than creating a second physical-only representation. Keep the first slice to the compiler-proven bounded integer members.
4. Materialize every explicit union member view from the same root-owned immutable bytes at offset zero while preserving each compiler-described width/signedness and exact core/runtime-artifact provenance. Do not infer an active member.
5. Make `CoreInspectionSession::inspect_union_member()` context-neutral: physical and selected-inline roots resolve source ownership separately, then share one explicit bounded union-member selector.
6. Prove real `mdbg-core` behavior for `frame 1 -> inline physical -> print <union> -> union-member <union> <signed-member> -> union-member <union> <unsigned-member>`, deterministic missing-member rejection, and thread/frame invalidation.
7. Keep active-member inference, discriminators/variant parts, pointer-member variants without evidence, recursive/nested unions, arbitrary reinterpretation, mutation, and expression-language semantics out of scope.
8. Require full GCC / Clang-large CI plus the relevant permanent GCC / Clang compiler/oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion continues outward with executable historical-frame capability rather than returning to fixed-array shape farming.
