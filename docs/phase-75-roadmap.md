# Phase 75 Roadmap — historical live pointer-member to bounded structure reachability

Status: complete for the current Linux x86-64 optimized historical live-frame milestone.

Phase 75 extends the fixed-depth historical live traversal model by one compiler-proven structural layer. A pointer-valued direct member may now carry a shallow bounded descriptor for either the previously-supported terminal integer or one flat integer-only structure pointee, and selecting that member composes back into the existing canonical root descriptor before the existing live dereference path reads stopped-tracee memory.

## Completed acceptance

- A dedicated optimized GCC/Clang fixture retains a genuine caller-owned `HistoricalLiveNested *historical_nested_pointer` across a callee stop.
- The independent oracle proves the root pointer, outer sixteen-byte `HistoricalLiveNested{direct@0:u32, linked@8:HistoricalLiveLeaf*}`, inner eight-byte `HistoricalLiveLeaf{terminal@0:i32, marker@4:u32}`, pointer widths, caller return PC, active historical location range, and compiler-produced single-register root ownership before product assertions run.
- Test-first head `238d314049b9c455d5d087208fb2726767bc32a4` keeps both dedicated compiler-evidence lanes green and all pre-existing behavior green, then fails only in the two new PIE/non-PIE integrations because direct pointer-member typing still routes the structure pointee into the scalar-only typedef/const/base-type resolver.
- Exact production head `003869404a2cdb37af253972f731bcec4b31cccc` expands the intentionally shallow aggregate-member pointee descriptor without introducing a recursive generic type graph:
  - scalar pointees retain byte width / signedness and `Integer` kind;
  - structure pointees carry only byte extent plus a finite list of direct integer-only member descriptors.
- One ownership-neutral `resolve_bounded_pointer_member_pointee_type()` handles typedef/const traversal and accepts exactly an integer scalar or one bounded flat integer-only structure. Pointer-valued/nested/enum/array/union/bit-field children inside that second structure remain rejected.
- `resolve_bounded_direct_structure_member_type()` reuses that shallow resolver for pointer-valued direct members. No historical-only parser or member type system is introduced.
- The existing live root-pointer metadata validator accepts the new shallow structure-pointee shape only when all child members are bounded direct integers. The Phase 74 signed-int32 terminal pointee remains independently accepted.
- `inspect_local_aggregate_member()` converts the shallow selected member descriptor into the already-canonical `LocalValueType` only at the explicit member-selection boundary.
- `dereference_local_pointer(Debugger, InspectionFrameContext, LocalScalarValue)` remains unchanged and performs the second stopped-tracee memory read through the existing bounded live structure decoder.
- API evidence proves:
  - root pointer -> outer sixteen-byte structure;
  - explicit `linked` pointer-member selection;
  - exact eight-byte `HistoricalLiveLeaf` pointee metadata;
  - explicit second dereference to `terminal = 0x02468ace` and `marker = 0x89abcdef`;
  - deterministic null-member and stale-frame rejection.
- Traversal remains fixed depth. There is no pointer chain engine, recursive object graph, implicit dereference, arbitrary member path, pointer arithmetic, cast, mutation, array/union traversal, or expression grammar.
- Exact production head `003869404a2cdb37af253972f731bcec4b31cccc` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler-evidence lanes, based directly on main `aadd2418aea67021d3c80e9dd7b45af8b3fd176a`.

Phase 75 is sealed here. A third pointer depth, another leaf layout, alternate scalar values, or more pointer-valued members are not new milestones.

## Phase 76 promotion — compiler-proven current-frame live fixed-array ownership and checked indexing

The next meaningful frontier leaves the historical pointer chain entirely. Canonical fixed-array typing, bounded element metadata, byte materialization, rendering, and checked indexing already exist across immutable core and selected-inline ownership. Current live ptrace lookup, however, still resolves an array root and then explicitly rejects it with `live fixed-array materialization is outside current compiler evidence`.

The first coherent Phase 76 slice must:

1. Start from a genuine optimized GCC and Clang current-frame fixture with one small fixed array of deterministic integer elements that remains live at an exact debugger stop.
2. Independently prove the real `DW_TAG_array_type`, element count/width/signedness, total extent, lexical ownership, exact stop PC, and compiler-produced active location expression before product assertions run.
3. Accept only the concrete location form(s) emitted by both permanent compiler lanes. If the compiler chooses stack memory, reuse the existing live frame-base/address machinery; if it chooses register pieces, minimally reuse the existing bounded register-piece reconstruction. Do not speculate toward arbitrary register numbers, mixed pieces, or a general DWARF VM.
4. Reuse canonical `LocalValueType::array_type` and the existing bounded array byte materializer/indexer. No live-only array descriptor, parser, decoder, or index implementation is allowed.
5. Preserve live debugger ownership and stop freshness. The array object must come only from the stopped tracee's compiler-owned register/memory state and must become invalid after execution advances.
6. Add an explicit live API/indexing workflow and, if the live CLI does not yet expose checked array indexing, extend it coherently so a real `mdbg` session can perform `print <array>` and one explicit `array-element <array> <index>` operation.
7. Prove exact values plus deterministic out-of-range rejection under PIE and non-PIE, GCC and Clang-large.
8. Keep multidimensional/VLA/flexible arrays, decay-to-pointer, slices, mutation, arbitrary expressions, unsupported element kinds, and unsupported location forms out of scope.
9. Require full GCC / Clang-large CI plus permanent compiler oracle/API/CLI evidence on the exact candidate before integration.

This promotion returns to a new current-frame live typed-object capability instead of farming additional historical pointer depth.
