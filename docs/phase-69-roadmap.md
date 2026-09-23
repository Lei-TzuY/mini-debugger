# Phase 69 Roadmap — canonical bounded root type resolution

Status: complete for the current Linux x86-64 bounded typed-value architecture milestone.

Phase 69 removes the selected-inline root-kind dispatch ladder exposed by Phase 68. Ordinary snapshot inspection and selected-inline inspection now resolve the same canonical root `LocalValueType` plus optional bounded pointer-pointee metadata through one ownership-neutral result before location ownership/materialization diverges.

## Completed acceptance

- One private `BoundedRootTypeResolution` carries the canonical root `LocalValueType` plus optional `LocalValueType` pointee metadata required by the existing one-hop pointer traversal.
- One shared `resolve_bounded_root_value_type()` owns root type choice for both immutable ordinary snapshot lookup and selected-inline lookup:
  - 4/8-byte compiler-proven floating roots are recognized first through the existing floating resolver;
  - pointer roots reuse the existing bounded integer/structure pointee resolver and preserve exact pointee metadata;
  - all remaining integer, structure, union, fixed-array, and enum roots reuse canonical `resolve_value_type()`.
- Ordinary snapshot inspection deletes its previous floating→pointer→ordinary three-way dispatch and consumes the shared root result directly.
- Selected-inline inspection deletes its hand-written floating→pointer→structure→union→array→enum→scalar priority ladder. Root kind, enum metadata, array metadata, union/structure members, floating identity, and pointer pointee metadata now arrive through the same shared result used by ordinary snapshot inspection.
- Frame-zero register-piece dispatch is now based on canonical `LocalValueKind` instead of retaining `direct_structure`, `direct_array`, and `direct_union` discovery booleans.
- The old selected-inline structure type re-resolution is removed. A validation-only boundary preserves the selected-inline fail-closed duplicate-member-name rule without reparsing/rebuilding the root type.
- Location-expression ownership and materialization remain unchanged: caller/frame-zero fbreg ownership, RDX/RCX register/register-piece paths, XMM snapshot handling, frame-base computation, immutable memory reads, provenance, and selection invalidation keep their established behavior.
- No new source type, DWARF location form, recursive graph, expression language, pointer depth, or materialization capability is introduced by this phase.
- Exact implementation head `8fcddce51023b652eacb954f8659aa930a4872e1` passes full GCC / Clang-large CI and both dedicated GCC / Clang compiler/core/session/CLI evidence lanes, based directly on main `8262b519e47539e71ce3a0692805b588c7cbe879`.
- The implementation is net consolidating: `src/dwarf/source_lookup.cpp` removes the 112-line selected-inline root dispatch/re-resolution surface while preserving bounded validation and kind-based ownership dispatch.

Phase 69 is sealed here. Reintroducing per-context root-kind priority ladders, adding another wrapper alias around the same canonical result, or inventing type variants merely to exercise the resolver would regress the architecture.

## Phase 70 promotion — compiler-proven live enum identity

The next higher-value frontier crosses a subsystem boundary rather than extending immutable-core variants. Canonical bounded enum identity, enumerator tables, symbolic ambiguity handling, and CLI rendering are mature in post-mortem inspection, while live ptrace source-value lookup still explicitly rejects `LocalValueKind::Enumeration` before otherwise-supported register/stack scalar materialization.

The first coherent Phase 70 slice must:

1. Start from genuine optimized GCC and Clang live-debugger artifacts where one named enum local remains active at an exported probe through a compiler-produced location already owned by the live debugger. Do not hand-author enum DWARF or force a synthetic location.
2. Independently prove the real `DW_TAG_enumeration_type`, exact width/signedness representation, finite enumerator name/value table, active probe PC, and concrete compiler-produced location before product assertions run.
3. Reuse canonical `resolve_value_type()` enum metadata and the existing live register/stack location evaluator. Do not create a live-only enum parser, descriptor, or symbolic table.
4. Preserve root `LocalValueKind::Enumeration`, exact raw numeric value, `LocalEnumType`, conservative symbolic lookup, and deterministic duplicate-alias ambiguity. Unknown raw values must remain numeric.
5. Attach enum metadata through the live materialization path rather than flattening the value to a plain integer. Existing integer/pointer semantics and mutable-process ownership remain unchanged.
6. Prove the real live debugger API and CLI can stop at the genuine probe and render the enum symbol plus numeric value, then continue/exit cleanly under both permanent compiler lanes.
7. Keep enum mutation, arithmetic, flags decomposition, scoped-name reconstruction, enum-valued aggregates, arrays/unions, new register opcodes, and unrelated location-expression widening out of scope.
8. Require full GCC / Clang-large CI plus focused genuine compiler/API/CLI evidence on the exact candidate before integration.

This promotion uses the canonical typed-value architecture to expand an actually distinct execution mode: live debugger inspection.
