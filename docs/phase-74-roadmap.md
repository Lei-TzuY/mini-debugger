# Phase 74 Roadmap — historical live traversal through one pointer-valued direct member

Status: complete for the current Linux x86-64 optimized historical live-frame milestone.

Phase 74 extends the Phase 73 live historical pointer-to-structure path through one explicit pointer-valued direct member and one additional bounded terminal dereference. The traversal remains fixed-depth, compiler-described, and driven by the stopped tracee's current memory without introducing a recursive object graph or expression language.

## Completed acceptance

- A dedicated optimized GCC/Clang fixture retains a genuine caller-owned `HistoricalLiveLinked *historical_member_pointer` across a callee stop.
- The compiler-proven pointee is exactly a 16-byte `HistoricalLiveLinked` with `direct@0 : uint32_t` and `linked@8 : int32_t*`.
- An independent readelf oracle proves the root pointer type, pointee byte extent, direct member names/offsets/types, linked pointer width, linked signed-int32 pointee metadata, caller return PC, active historical location range, and the compiler-produced single-register location before product assertions run.
- Test-first head `db1623bf401c882e1c2a835350d799e91f78d2fb` keeps the dedicated compiler evidence green and exposes the unsupported product boundary in normal CI.
- Production head `0a7fe34a4e9b08f987070e5c9d83bbc6bbb0a17f` extends the already-canonical bounded direct-member model to preserve one pointer-valued direct member and its shallow signed-int32 pointee metadata.
- The containing object is still materialized by the existing live structure decoder. No historical-only structure decoder, pointer-member parser, or memory reader is introduced.
- `inspect_local_aggregate_member()` explicitly selects `linked` from the already-materialized live object and preserves pointer kind, width, raw address, and bounded pointee metadata.
- The existing `dereference_local_pointer(Debugger, InspectionFrameContext, LocalScalarValue)` path performs exactly one additional stopped-tracee read and yields the signed-int32 terminal value `0x02468ace`.
- Null member pointers, malformed pointee metadata, unreadable/short memory, and stale historical frames remain deterministic failures.
- Traversal depth remains fixed: root pointer -> one structure -> one pointer-valued direct member -> one terminal integer. Pointer chains, arbitrary member paths, recursion, pointer arithmetic, casts, implicit dereference, mutation, arrays, unions, bit fields, and nested object graphs remain outside this phase.
- Exact production head `0a7fe34a4e9b08f987070e5c9d83bbc6bbb0a17f` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler-evidence lanes, based directly on main `578e620217613ca6839280d7252d5cfe0b908e2a`.

Phase 74 is sealed here. More scalar pointee values, alternate member offsets, additional pointer fields, or another historical register are not new milestones.

## Phase 75 promotion — historical live pointer-member to bounded structure reachability

The next meaningful frontier is not another terminal scalar variant. Phase 74 proves the fixed traversal architecture, but `LocalStructMemberType::pointee_type` is still intentionally shallow and scalar-only. The next executable capability is one compiler-proven direct pointer member whose target is itself a bounded flat structure.

The first coherent Phase 75 slice must:

1. Start from genuine optimized GCC/Clang live evidence where a caller-owned historical root pointer targets one bounded structure containing:
   - one direct scalar member; and
   - one direct pointer member whose pointee is a second bounded flat structure with direct integer members only.
2. Independently prove both structure layouts, all member offsets/types, both pointer widths, root historical location ownership, runtime object addresses, linked target address, and terminal member values before product assertions run.
3. Reuse canonical root/direct-member type resolution rather than introducing a historical-only nested pointer-member parser. Any representation change needed to carry a structure pointee must be ownership-neutral and bounded.
4. Keep the second pointee strictly flat: direct integer members only, no nested structures, pointer-valued members, enums, bit fields, arrays, unions, or recursive/self-referential type graphs.
5. Provide one explicit pointer-member selection and one explicit dereference into the second bounded structure. The returned object must use the existing live structure decoder and stopped-tracee memory reader.
6. Preserve fixed traversal depth, stale-frame rejection, null/unreadable/malformed-metadata failures, and exact runtime provenance.
7. Do not add pointer arithmetic, casts, arbitrary dotted paths, implicit dereference, recursion, mutation, or a general object browser.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler/oracle/live inspection evidence on the exact candidate before integration.

This promotion deepens historical live typed-object reachability by one compiler-proven structural layer without relaxing the project's explicit boundedness model.
