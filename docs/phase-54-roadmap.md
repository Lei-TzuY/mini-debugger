# Phase 54 Roadmap — one canonical bounded direct-structure member resolver

Status: complete for the current Linux x86-64 bounded typed-value architecture milestone.

Phase 54 removes the remaining duplicate direct-structure member typing logic exposed by Phase 53. Ordinary physical root structures, selected-inline direct structures, and snapshot structure pointees now share one ownership-neutral resolver for the overlapping integer / pointer / enum member surface.

## Completed acceptance

- One shared `resolve_bounded_direct_structure_member_type()` owns direct-member name/type/constant-offset validation, bounded typedef/const traversal, integer resolution, x86-64 pointer width + bounded integer pointee metadata, enum identity, and aggregate storage bounds.
- Ordinary `resolve_value_type()` keeps the already-proven bit-field and one-hop nested-structure gates, then delegates ordinary direct members to the shared resolver.
- Selected-inline direct structure typing keeps the same bit-field and nested-structure gates, then delegates the ordinary direct member case to the same shared resolver.
- Snapshot pointer-to-structure pointee typing no longer owns a second `resolve_snapshot_struct_member_type()`; that duplicate implementation is deleted and the pointee path consumes the canonical resolver directly.
- No conversion adapter or parallel legacy resolver remains. On the exact implementation candidate, `resolve_snapshot_struct_member_type` has zero occurrences and the canonical direct-member resolver has exactly the expected definition + three consumer sites.
- The refactor preserves the existing bounded capability surface rather than silently widening it: bit-field and nested-structure handling remain explicit, mutable live-process enum/bit-field fail-closed guards remain intact, and no recursive generic object graph is introduced.
- Existing integer, pointer-valued, enum-valued, bit-field, nested-structure, pointer-to-structure, selected-inline, historical physical, session, CLI, and provenance behavior remain covered by the permanent executable suites.
- Exact implementation head `95210215ccefb9f16d130eda30a40c84133c74ce` passes full GCC / Clang-large CI plus both GCC / Clang dedicated compiler/core/session/CLI evidence lanes.
- The candidate is based directly on main `82a09697fe12bada37813edad129dc595774ecd8`, ahead by one implementation commit and behind by zero.

Phase 54 is sealed here. Reintroducing a snapshot-only direct-member resolver, duplicating wrapper traversal, or adding more scalar/member variants solely to exercise the helper would regress the architecture.

## Phase 55 promotion — compiler-proven frame-zero selected-inline pointer ownership

The next higher-value gap is executable rather than representational. Historical caller-frame selected-inline pointers already materialize from compiler-produced `DW_OP_fbreg` and support one bounded integer dereference. Frame-zero selected-inline integer and enum register ownership also already exist. However, frame-zero selected-inline pointer materialization is still explicitly rejected before the mature pointer traversal path can be used.

The first coherent Phase 55 slice must:

1. Start from genuine GCC and Clang optimized crash-frame artifacts where an active selected-inline local is a pointer with a compiler-produced frame-zero location. Do not hand-author a location expression or force product behavior before the compiler oracle proves the actual register/location form.
2. Independently prove inline lexical ownership, exact pointer type/width, bounded integer pointee metadata, selected physical PC, compiler-produced location form, and the pointee value/address before product assertions run.
3. Extend only the already-evidenced frame-zero selected-inline evaluator forms required by those artifacts. Reuse the selected physical thread's immutable crash register snapshot and existing snapshot-memory reader; selecting an inline context must not create a fictional register file, CFA, or memory image.
4. Materialize the pointer through the canonical `LocalValueType` / pointer metadata path and reuse the existing context-neutral one-hop pointer dereference primitive. No second pointer descriptor or inline-only dereference engine is allowed.
5. Preserve exact pointer-object and pointee provenance. Register/computed pointer ownership must remain distinguishable from the pointee's immutable core/runtime-artifact storage.
6. Prove real `mdbg-core` behavior for `inline <active> -> print <pointer> -> deref <pointer>` on physical frame zero, including deterministic null/unsupported-pointee rejection and invalidation after thread/frame/inline selection changes.
7. Keep pointer arithmetic, pointer chains, implicit dereference, arbitrary expressions, mutation, recursive object graphs, and unsupported compiler location forms out of scope.
8. Require full GCC / Clang-large CI plus permanent compiler oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion returns from architecture consolidation to a genuine source-value capability frontier already exposed by an explicit product guard.
