# Phase 47 Roadmap — one canonical bounded root value type

Status: complete for the current Linux x86-64 bounded typed-value architecture milestone.

Phase 47 removes the root-type duplication left after Phase 46. Direct DWARF values and pointer pointees previously used two structurally equivalent descriptors: private `ValueType` and exported/internal `LocalPointeeType`. Both carried byte size, signedness, semantic kind, and the same canonical `LocalStructMemberType` member graph.

## Completed acceptance

- The duplicate private `ValueType` definition is deleted.
- `LocalPointeeType` is renamed to the ownership-neutral `LocalValueType`; direct values and pointer pointees now use the same bounded root descriptor.
- Ordinary DWARF value resolution, floating-value resolution, selected-inline direct structure/union descriptions, snapshot materialization, live/register-piece structure decoding, direct pointer dereference, pointer-to-structure traversal, and aggregate helper code all consume `LocalValueType`.
- `LocalScalarValue::pointee_type` retains exactly the same bounded semantics, but its descriptor is no longer named or shaped as a pointer-only representation.
- The shallow `LocalPointerPointeeType` remains intentionally distinct for terminal pointer-valued aggregate members. It carries only bounded scalar pointee width/signedness and does not create a recursive generic type graph.
- No adapter alias, conversion shim, or parallel legacy descriptor remains in the migrated code: `ValueType` and `LocalPointeeType` have zero remaining code occurrences on the exact candidate.
- Machine-state ownership remains unchanged. Live ptrace values, immutable core frames, runtime-artifact fallback, and selected-inline lexical ownership still use their existing evaluators and provenance; only type identity is shared.
- Existing executable coverage proves unchanged scalar, floating, structure, pointer-to-integer, pointer-to-structure, pointer-valued member, register-piece aggregate, selected-inline aggregate, session, and CLI behavior.
- The exact production head passes full GCC / Clang-large CI and the dedicated GCC / Clang selected-inline evidence workflow.

Phase 47 is sealed here. Further naming churn or wrapper aliases around the same four root fields are not architectural progress.

## Phase 48 promotion — compiler-proven physical nested aggregate composition

The next higher-value gap is executable rather than representational. Selected-inline inspection already proves exactly one nested by-value structure boundary, while a historical physical frame still accepts only flat direct members. With one canonical root/member type graph in place, the project can now close that context gap without adding another parallel type system.

The first coherent Phase 48 slice must:

1. Start from a genuine GCC and Clang core artifact where a historical physical frame owns one stack-resident outer structure through compiler-produced `DW_OP_fbreg`, and that outer structure contains exactly one direct by-value inner structure with a bounded terminal scalar. Reuse the existing historical caller fixture if compiler retention remains stable; do not synthesize DWARF.
2. Independently prove the physical subprogram, outer/inner byte sizes, direct member names, constant offsets, terminal scalar type, selected lookup PC, and frame-base/location forms before product assertions run.
3. Extend the canonical ordinary structure type resolver to describe exactly one nested structure-valued direct member using `LocalStructMemberType::members`. Prefer sharing/extracting the already-proven bounded nested-member logic rather than creating a second physical-only resolver.
4. Materialize the inner structure strictly from bytes already owned by the outer aggregate. Preserve compiler-owned outer and terminal offsets and immutable core/runtime-artifact provenance; do not perform an unrelated memory search for the inner object.
5. Make `CoreInspectionSession::inspect_nested_aggregate_member()` context-neutral in the same style as direct aggregate traversal: physical and selected-inline roots resolve ownership separately, then share the bounded materialized-value traversal primitive.
6. Prove real `mdbg-core` behavior for a workflow such as `frame 1 -> inline physical -> print <outer> -> nested-aggregate-member <outer> <inner> <terminal>`, including deterministic invalidation after frame/thread changes.
7. Keep nesting depth explicit and finite at exactly one inner structure boundary. Reject a structure-valued terminal, arbitrary dotted paths, recursion, cycles, inheritance, dynamic member locations, flexible arrays, mutation, and implicit dereference.
8. Require full GCC / Clang-large CI plus the relevant permanent compiler/oracle/core/session/CLI evidence on the exact candidate head before integration.

This promotion uses the canonical type graph to add a real historical-frame capability rather than continuing architecture-only cleanup.
