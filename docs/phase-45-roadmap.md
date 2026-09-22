# Phase 45 Roadmap — context-neutral bounded typed aggregate traversal

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 45 removes the remaining context split in bounded direct aggregate traversal. Physical and selected-inline roots still resolve lexical ownership through their own existing lookup paths, but once a bounded `LocalScalarValue` structure is materialized, direct member selection and one pointer-member dereference use the same typed-value traversal contract.

## Completed acceptance

- The permanent historical caller fixture now contains a genuine stack-resident `CallerTypedAggregate { uint64_t *payload; uint64_t marker; }` plus a stack-owned payload target. Both GCC and Clang preserve the aggregate through the already-proven historical `DW_OP_fbreg` ownership family.
- An independent readelf-based oracle proves the exact aggregate location, 16-byte structure size, `payload@0` pointer-to-unsigned-64 type, and `marker@8` unsigned-64 type before product assertions execute.
- Test-first evidence failed in both compiler lanes at the intended architecture boundary: the physical aggregate type decoder treated every direct structure member as an integer-only `AggregateMemberType` and rejected the compiler-produced pointer member.
- The shared aggregate member descriptor now retains member kind plus bounded pointer-pointee metadata. Integer-only aggregate layout remains the default, but one compiler-proven pointer-valued direct member can carry its x86-64 storage width and bounded integer pointee identity through the same `ValueType` model.
- Both live/snapshot structure decoders materialize the member kind, relative offset, and pointer metadata directly. Snapshot pointer-to-structure dereference no longer needs a post-materialization metadata patch-up loop; the type model itself is authoritative.
- Direct aggregate member selection is now a context-neutral operation over an already-materialized bounded structure. The compatibility selected-inline entry point delegates to the same primitive rather than maintaining another traversal implementation.
- One-hop aggregate pointer-member dereference is likewise context-neutral: it selects the member through the shared traversal primitive, validates the bounded integer pointee, performs exactly one snapshot-memory read, and reports the pointee read's own immutable provenance.
- `CoreInspectionSession::inspect_aggregate_member()` and `dereference_aggregate_member()` now differ only in how the root value is resolved. Physical roots use ordinary snapshot lookup; selected-inline roots use the selected inline DIE ownership path; traversal after materialization is shared.
- Real API evidence proves historical physical `caller_typed_aggregate.payload` selection and dereference to `0x7766554433221100`, while retaining exact core provenance and invalidation across thread/frame changes.
- The real `mdbg-core` workflow proves `frame 1 -> print caller_typed_aggregate -> aggregate-member caller_typed_aggregate payload -> deref-aggregate-member caller_typed_aggregate payload`.
- The exact production head passes the full permanent GCC / Clang-large CI matrix and the dedicated GCC / Clang selected-inline evidence workflow.

Phase 45 is sealed here. More pointer fields, alternate offsets, pointer target widths, or additional scalar marker layouts are not new milestones. Arbitrary recursive paths, implicit dereference, pointer arithmetic, mutation, inheritance, and a general C/C++ expression parser remain outside this phase.

## Phase 46 promotion — one canonical bounded aggregate type descriptor

The next architectural gap is descriptor duplication, not another traversal command. The project now has a richer `LocalStructMemberType` used by snapshot/selected-inline typed values and a separate internal `AggregateMemberType` used by the older value decoder. Phase 45 had to enrich the latter to preserve pointer identity, demonstrating that parallel type descriptors are becoming an integration liability.

The first coherent Phase 46 slice must:

1. Audit every producer/consumer of `AggregateMemberType`, `LocalStructMemberType`, `ValueType`, and `LocalPointeeType` across live debugging, immutable snapshot inspection, pointer dereference, register-piece aggregates, and selected-inline materialization.
2. Establish one canonical bounded direct-member descriptor for name, constant byte offset, storage width/signedness, semantic kind, and optional bounded pointee/enum metadata. Do not broaden supported recursion merely because the descriptor becomes shared.
3. Migrate at least one cross-layer producer and all of its consumers to that canonical representation, deleting the redundant metadata conversion or post-processing path rather than adding adapters in both directions.
4. Preserve the current hard bounds: 256-byte aggregate, 32 direct members, constant offsets, explicit pointer width, bounded pointee metadata, fail-closed unsupported member kinds, and immutable provenance.
5. Keep live ptrace and core-snapshot machine-state ownership separate. Type descriptor unification must not cause snapshot operations to borrow live state or live operations to consume core-only provenance.
6. Prove the canonical descriptor through existing integer structures, the Phase 45 pointer-valued direct member, pointer-to-structure dereference, and register-piece aggregate regressions under both permanent compiler lanes.
7. Add a new executable capability only if genuine compiler evidence exposes a capability currently blocked specifically by the duplicate descriptor boundary. Do not manufacture enum/array/union variants solely to justify the refactor.
8. Require full CI and dedicated compiler evidence on the exact candidate head before integration.

This promotion pays down a real architecture boundary exposed by executable Phase 45 evidence, so the next expansion can build on one typed aggregate model instead of extending two nearly-parallel ladders.
