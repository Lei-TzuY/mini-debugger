# Phase 73 Roadmap — historical live pointer to bounded structure reachability

Status: complete for the current Linux x86-64 optimized historical live-frame milestone.

Phase 73 composes Phase 72's CFI-owned historical pointer identity with the existing canonical bounded structure type/decoder. A caller-owned historical pointer can now reach one flat, compiler-described live object in the stopped tracee without introducing a historical-only object model.

## Completed acceptance

- A dedicated optimized GCC/Clang fixture retains a genuine caller-owned `HistoricalLivePair *historical_structure_pointer` across a callee stop.
- `HistoricalLivePair` is compiler-proven as an exact 16-byte flat structure with `count@0 : uint32_t` and `delta@8 : int64_t`.
- Independent readelf evidence proves pointer width, pointer-to-structure type graph, complete member layout, caller return PC, active historical location range, and the compiler-produced register location before product assertions run.
- Test-first exact head `04c08e9dfb43a5c9ede93c8bcb6bb402e4ed9a02` keeps all pre-existing behavior green under GCC and Clang; both new PIE/non-PIE integrations fail only at the intended product boundary: `live pointer received unsupported bounded pointee metadata`.
- Production head `30fe5890f8ec86bae8405dfd8a4e14ec77f28725` expands canonical live pointer metadata acceptance only to a bounded flat structure whose direct members are integer scalars with checked widths/offsets and no nested/pointer/bit-field/enum metadata.
- The existing `resolve_bounded_root_value_type()` remains authoritative for pointer and structure identity. No historical-only structure parser, pointer descriptor, or memory reader is introduced.
- The existing live `dereference_local_pointer()` retains the terminal integer path and adds one Structure branch that reads exactly the pointee byte extent from the stopped tracee and delegates materialization to the existing `decode_structure()`.
- Unsupported pointee kinds, malformed layout, null pointers, host-width overflow, short/unreadable memory, and stale inspection frames remain deterministic failures.
- API evidence proves exact pointer identity, 16-byte bounded structure metadata, one-hop dereference to a Structure value, `count == 0x11223344`, and signed `delta == -123456789`.
- Execution advancing to a new stop invalidates the old historical frame before a second dereference can borrow stale machine state.
- Exact production head `30fe5890f8ec86bae8405dfd8a4e14ec77f28725` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler/core/session evidence lanes, based directly on main `4cfeda96eaf99c41713d5a82f5f0c0d28091ceb2`.

Phase 73 is sealed here. More integer members, alternate structure sizes, another historical register, nested structures, arrays, or pointer arithmetic are not new milestones.

## Phase 74 promotion — historical live object traversal through one pointer-valued member

The next executable frontier is controlled depth-two reachability, not another flat-structure variant. Existing canonical direct-member metadata already supports a pointer-valued member with a bounded scalar pointee, and immutable snapshot inspection already proves this traversal shape. The live historical path should compose those existing pieces without becoming a recursive expression engine.

The first coherent Phase 74 slice must:

1. Start from genuine optimized GCC/Clang live evidence where a caller-owned historical root pointer targets one bounded flat structure containing:
   - one direct scalar member; and
   - one direct pointer member whose pointee is a bounded signed-int32 terminal.
2. Independently prove root pointer ownership, structure byte extent, direct member names/offsets/types, pointer-member x86-64 width, pointer-member signed-int32 pointee metadata, target object address, linked target address/value, caller lookup PC, and active historical location before product assertions run.
3. Reuse `InspectionFrameContext` ownership and canonical `resolve_bounded_root_value_type()` / `LocalStructMemberType` metadata. No second structure-member type system or historical-only pointer-member parser is allowed.
4. Extend the live bounded structure-pointee acceptance only to the exact already-representable direct pointer-member shape. Keep bit fields, enums, nested structures, arrays, unions, and arbitrary pointer mixtures outside this slice.
5. Provide one explicit live pointer-member selection operation over the already-materialized containing object, preserving member name/kind/width/raw pointer metadata.
6. Provide exactly one additional bounded dereference for that pointer-valued member into a terminal signed-int32 value using stopped-tracee memory.
7. Keep overall traversal depth fixed. No pointer chains beyond the proven member hop, arbitrary member paths, pointer arithmetic, casts, implicit dereference, recursive graphs, or mutation.
8. Preserve stale-frame rejection and deterministic null/unreadable/malformed-metadata failures.
9. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler/oracle/live inspection evidence on the exact candidate before integration.

This promotion expands historical live object reachability through a typed direct member while keeping traversal explicit, bounded, and evidence-driven.
