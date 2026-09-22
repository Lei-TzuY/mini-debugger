# Phase 46 Roadmap — one canonical bounded aggregate member descriptor

Status: complete for the current Linux x86-64 bounded typed-value architecture milestone.

Phase 46 removes the duplicate direct-member descriptor exposed by Phase 45. The older internal `AggregateMemberType` and the richer public/internal `LocalStructMemberType` described the same bounded member ownership with overlapping fields, forcing pointer metadata to be reconstructed when values crossed parser, materializer, and pointer-dereference layers.

## Completed acceptance

- `AggregateMemberType` is deleted. `ValueType::members` now directly stores `LocalStructMemberType`.
- The ordinary DWARF structure resolver produces the canonical descriptor for integer and compiler-proven pointer-valued direct members, preserving constant offset, storage width, signedness, semantic kind, and bounded pointee metadata.
- Live/register-piece structure decoding and immutable snapshot-memory structure decoding consume the same canonical descriptor fields. Materialization preserves relative offset plus available pointee, bit-slice, and enum metadata rather than translating through a reduced intermediate member type.
- Snapshot pointer-to-structure dereference no longer rebuilds a second member descriptor. After the existing hard validation of byte range, kind, pointer width, and bounded pointee metadata, the already-validated `LocalStructMemberType` is reused directly by `ValueType`.
- Machine-state ownership is unchanged: live ptrace decoding still uses live state; immutable core inspection still uses snapshot/core/runtime-artifact provenance. Descriptor unification does not cross that boundary.
- The existing executable matrix proves the refactor across integer structures, Phase 45 pointer-valued direct members, pointer-to-structure dereference, register-piece aggregate decoding, selected-inline typed aggregates, and real session/CLI workflows.
- No new enum/array/union/member-count variant was manufactured to justify the refactor. This phase is an architecture consolidation milestone with preserved executable behavior.
- The exact production candidate passes full GCC / Clang-large CI and the dedicated GCC / Clang selected-inline evidence workflow.

Phase 46 is sealed here. Reintroducing a second member descriptor, conversion adapter, or post-materialization metadata patch would regress the architecture.

## Phase 47 promotion — canonical bounded root type descriptor

The next duplication sits one level above members. Internal `ValueType` and `LocalPointeeType` now both describe a bounded root type using byte size, signedness, semantic kind, and canonical direct-member metadata. Their remaining split causes root values and pointer pointees to travel through near-parallel representations even though Phase 46 unified their member graph.

The first coherent Phase 47 slice must:

1. Audit every producer and consumer of `ValueType` and `LocalPointeeType`, including ordinary/local DWARF resolution, snapshot pointer metadata, live register-piece aggregates, snapshot materialization, direct pointer dereference, and selected-inline typed values.
2. Establish one canonical bounded root type descriptor for byte size, signedness, semantic kind, and `LocalStructMemberType` members. Naming and placement must reflect that it is usable for both direct values and pointees rather than privileging either ownership path.
3. Migrate one complete cross-layer path and then all dependent consumers to the canonical root descriptor, deleting redundant reconstruction rather than introducing bidirectional adapters.
4. Preserve existing hard limits and fail-closed semantics: aggregate byte/member bounds, explicit x86-64 pointer width, bounded integer pointees, unsupported-kind rejection, null/address checks, and immutable provenance.
5. Keep type identity separate from machine-state ownership. A shared type descriptor must not make live and snapshot evaluators share registers, frame bases, memory, or provenance.
6. Prove unchanged executable behavior for scalar values, structure locals, pointer-to-integer, pointer-to-structure, typed pointer members, register-piece aggregates, and selected-inline aggregates in the permanent compiler lanes.
7. Add a new user-visible capability only if genuine compiler evidence shows that the duplicate root descriptor itself blocks it. Do not broaden recursive type graphs merely because the representation becomes shared.
8. Require full CI and dedicated compiler evidence on the exact candidate head before integration.

This promotion continues the same direction: one bounded type graph, multiple explicit ownership/evaluation contexts.
