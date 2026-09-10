# Phase 35 Roadmap — selected-inline bounded aggregate/member traversal

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 35 extends selected-inline post-mortem pointer reachability from one integer pointee to one bounded pointer-to-structure local and its direct members while preserving the invariant that the selected inline context owns only source/DIE scope, never a fictional machine frame.

## Completed acceptance

- A genuine cross-file `-O2 -g -gdwarf-4` fixture is compiled independently by permanent GCC and Clang evidence lanes as PIE and non-PIE. The selected `caller_inline_inner` context owns `caller_aggregate_pointer` through a compiler-produced concrete `DW_AT_location`; no hand-authored DWARF location expression is used.
- The compiler oracle resolves `caller_aggregate_pointer` through its real `DW_AT_type` chain to `DW_TAG_pointer_type -> DW_TAG_structure_type`, requires the `direct` and `linked` direct-member DIEs plus constant `DW_AT_data_member_location`, proves `direct` is an integer base type, and proves `linked` is a pointer to an integer base type. GCC and Clang both pass this evidence gate.
- The fixture makes the pointed object deterministic: `caller_inline_aggregate.direct == 0x11223344`, `caller_inline_aggregate.linked == &caller_inline_member_pointee`, and `caller_inline_member_pointee == 0x13579bdf`. The noinline crash leaf reads both members before producing the real SIGSEGV core, so the artifact is retained by ordinary C semantics rather than synthetic location machinery.
- Selected-inline member traversal reuses the existing bounded `LocalPointeeType` structure/member metadata produced by the physical snapshot DWARF parser. No second type system, recursive object graph, synthetic inline register file, inline CFA, or inline memory image is introduced.
- `CoreInspectionSession::inspect_pointer_member()` now dispatches through the selected inline DIE when an inline context is active. The direct integer member is read from immutable snapshot memory using the compiler-owned pointer value and exact bounded member offset, and retains core-memory or runtime-artifact provenance.
- Pointer-valued direct members retain their existing bounded integer-pointee metadata. `dereference_pointer_member()` performs exactly one additional immutable snapshot read, rejects null/host-width overflow/unsupported metadata, and materializes `caller_aggregate_pointer->linked` as the deterministic `0x13579bdf` integer pointee.
- Member-name ownership remains exact. Empty names, unknown members, out-of-range layout metadata, unsupported direct member kinds, malformed pointer metadata, unreadable snapshot bytes, and provenance failures remain explicit errors rather than guessed values.
- The selected inline DIE remains the sole lexical owner. Existing abstract-origin, active lexical-scope, shadowing/equal-depth ambiguity, module ownership, physical-frame ownership, and selected-thread validation continue to run before the inline pointer value can be materialized.
- A genuine core retains a sibling pthread so the product regression proves ownership invalidation, not merely value decoding: frame selection clears stale inline selection, `inline physical` returns to the physical context, and thread selection rebuilds the immutable thread trace and clears the selected inline owner.
- The permanent `core-inline-evidence` workflow first verifies the compiler artifact, then builds `mdbg-core` plus a real `CoreInspectionSession` integration. PIE and non-PIE workflows prove `frame 1 -> selected caller_inline_inner -> member caller_aggregate_pointer direct -> deref-member caller_aggregate_pointer linked` through both the API and CLI.
- Test-first exact head `fe7885707968406f5945220f762e60c52c94d090` built the compiler artifacts and core inspection surface successfully but failed the product gate exactly at the pre-Phase-35 physical-only guard: `inline-context pointer traversal is not supported; select physical first`. Exact production head `86bd9fe55f6f834ae4364bc711a7072f666aa57d` then passed the dedicated GCC/Clang compiler/core product gate and the normal permanent GCC/Clang-large Configure/Build/Test matrix.

Phase 35 is intentionally sealed here. More members, another structure layout, another fbreg offset, second-level pointer chains, or additional integer widths are not reasons to continue this phase. New object forms require a distinct compiler-produced artifact and a new ownership capability.

## Phase 36 promotion — selected-inline bounded direct aggregate values

The next higher-level typed-value gap is explicit in the selected-inline materializer itself. Selected inline contexts can now recover bounded integer scalars, pointer scalars, integer pointees, and direct members reachable through one pointer-to-structure local, but the materializer still categorically accepts only integer/pointer scalar values. A by-value structure local owned directly by the selected inline DIE cannot yet be materialized as a `LocalValueKind::Structure`.

The first coherent Phase 36 slice must:

1. Start from a genuine permanent-lane GCC and Clang optimized core in which the selected inline DIE owns one by-value structure local with a concrete compiler-produced location that can be proven from the immutable selected physical frame. Do not manufacture a structure location, force a convenient DWARF operation, or widen the evaluator before the artifact exists.
2. Keep machine-state ownership physical. Inline selection remains source/DIE scope only; register values, CFA/frame base, stack addresses, module ownership, aggregate bytes, and provenance must come from the selected immutable physical snapshot frame and existing snapshot-memory machinery.
3. Reuse the existing bounded `LocalValueKind::Structure`, aggregate/member layout, size/member-count limits, and direct integer/pointer member metadata. Do not create a separate inline aggregate representation.
4. Materialize exactly the compiler-proven by-value aggregate and expose it through `CoreInspectionSession::inspect_value()` and real `mdbg-core print`. If the compiler emits an already-supported stack/register-piece form, reuse it; if it emits a new form, support only the minimum form justified by both evidence and ownership semantics.
5. Preserve selected-inline lexical ownership, abstract-origin resolution, ambiguity/shadowing rejection, module/frame/thread invalidation, null/unreadable/optimized-out failures, and immutable core/runtime-artifact provenance.
6. Do not use Phase 36 as a vehicle for arrays, unions, inheritance, recursive/nested object graphs, arbitrary pointer arithmetic, general DWARF expression evaluation, or speculative optimizer-location coverage.
7. If the permanent compiler lanes do not retain a stable by-value structure binding within the current immutable physical-frame model, stop and record that compiler evidence rather than fabricating an aggregate or weakening the gate.

This promotion moves optimized post-mortem inspection from pointer-reachable object fields to direct typed aggregate value ownership while keeping every machine-state fact anchored in the genuine physical core frame.
