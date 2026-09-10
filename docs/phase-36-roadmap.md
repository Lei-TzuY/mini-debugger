# Phase 36 Roadmap — selected-inline bounded direct aggregate values

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 36 extends selected-inline post-mortem inspection from pointer-reachable aggregate fields to one by-value structure local owned directly by the selected inline DIE, while preserving the invariant that inline selection contributes source/DIE scope only and never invents machine state.

## Completed acceptance

- The permanent GCC and Clang optimized-core lanes retain a genuine `caller_direct_aggregate` local owned by selected `caller_inline_inner`. The compiler oracle proves a concrete `DW_TAG_structure_type` binding and compiler-produced `DW_OP_fbreg` location in both lanes: GCC uses `DW_OP_fbreg -48`, while Clang uses `DW_OP_fbreg +32`. No hand-authored location expression or convenience-only DWARF form is introduced.
- The aggregate is retained through the real deterministic crash and immutable historical physical frame 1. Register state, frame base/CFA, runtime address, module ownership, bytes, and provenance all remain owned by that physical snapshot frame; selecting an inline context does not create an inline register file, stack, CFA, or memory image.
- Selected-inline direct-structure detection reuses the existing bounded snapshot structure parser rather than adding an inline type system. Typedef/const wrappers, the 256-byte structure limit, 32-member limit, exact direct-member offsets, duplicate/member-layout checks, and existing failure semantics remain in force.
- The compiler-proven aggregate contains both member categories already supported by the bounded object model. `direct` materializes as the signed 32-bit `0x55667788` integer; `linked` materializes as an x86-64 pointer and retains its bounded signed-integer pointee metadata instead of being flattened into an integer.
- Historical selected-inline aggregate materialization remains limited to the compiler-proven `DW_OP_fbreg` path. The existing physical frame-base machinery computes the runtime address and immutable snapshot memory supplies exactly the aggregate byte width. Frame-zero selected-inline aggregates remain explicitly outside current compiler evidence.
- `CoreInspectionSession::inspect_value("caller_direct_aggregate")` returns the existing `LocalValueKind::Structure` representation with both direct members and immutable `SnapshotCoreMemory` or exact owner-matched `SnapshotRuntimeArtifact` provenance. No parallel aggregate representation is introduced.
- Real `mdbg-core` executes `frame 1 -> inline caller_inline_inner -> print caller_direct_aggregate` and renders the bounded by-value aggregate through the ordinary product surface. Existing selected-inline pointer-member traversal in the same workflow remains green, proving the new direct-value path does not regress Phase 35.
- Frame selection, `inline physical`, and thread selection continue to invalidate selected-inline ownership. Existing lexical ownership, abstract-origin resolution, active-scope/shadowing/equal-depth ambiguity checks, module ownership, physical-frame validation, selected-thread validation, unreadable/optimized-out failures, and immutable provenance remain unchanged.
- Test-first branch evidence established the gap before production: the normal permanent GCC/Clang-large suite was green while the dedicated compiler/core product gate failed only when the selected-inline materializer attempted to treat the by-value structure as a scalar type. Exact production head `57c2e33b3ae3f614e413f7030e3c41b6657e0239` then passed the complete normal GCC/Clang-large matrix and the dedicated GCC/Clang `core-inline-evidence` pipeline, including compiler binding, core retention, session materialization, and CLI rendering.

Phase 36 is intentionally sealed here. More structure layouts, member counts, integer widths, fbreg offsets, or compiler spellings are not reasons to continue this phase. Arrays, unions, inheritance, nested/recursive aggregate graphs, arbitrary pointer arithmetic, and general DWARF expression evaluation remain outside this bounded milestone unless a new compiler-produced artifact and ownership requirement justify a distinct phase.

## Phase 37 promotion — selected-inline direct aggregate member traversal

The next higher-value typed-object gap is now explicit at the session/product boundary. A selected-inline by-value structure can be materialized and printed with typed direct-member metadata, but `CoreInspectionSession` only exposes dedicated member traversal for a pointer-to-structure local. There is no typed operation that selects one member from the directly owned aggregate and, when that member is a pointer, performs the same single bounded dereference already available through the pointer-reachable object path.

The first coherent Phase 37 slice must:

1. Start from the same genuine GCC/Clang `caller_direct_aggregate` artifact unless a new compiler artifact is actually required. Do not manufacture another structure merely to vary layout or location.
2. Add one typed direct-aggregate member inspection path that consumes the already materialized `LocalValueKind::Structure` metadata. It must not reparse DWARF into a second member model or recompute machine-state ownership outside the selected physical snapshot frame.
3. Prove exact integer-member selection for `direct` and, because the genuine artifact already contains it, exact pointer-member selection for `linked` with its existing bounded integer-pointee metadata.
4. Permit at most the existing single bounded pointer dereference for the pointer-valued direct member, preserving null/host-width/unreadable/provenance failures. Do not generalize into recursive traversal, pointer chains, nested structures, arrays, unions, inheritance, or expression-language syntax.
5. Preserve selected-inline lexical/abstract-origin/shadowing/ambiguity/module/frame/thread ownership and all invalidation behavior before any member bytes or pointee bytes are exposed.
6. Prove the behavior through both `CoreInspectionSession` and real `mdbg-core`, using a command/API surface that distinguishes direct aggregate membership from the existing pointer-to-structure member path rather than silently changing old command semantics.

If the already materialized aggregate metadata cannot support this traversal without duplicating the type system or weakening ownership/provenance checks, stop and record that architectural constraint before adding a new abstraction.
