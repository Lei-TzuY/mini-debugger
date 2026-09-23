# Phase 81 Roadmap — explicit live historical inspection-frame selection

Status: complete for the current Linux x86-64 live multi-frame source-inspection milestone.

Phase 81 closes the ownership-selection gap between the already-proven historical live InspectionFrameContext machinery and the interactive mdbg CLI. Users can now explicitly select a freshly CFI-recovered inspection frame for source-value commands without mutating execution state or inventing caller machine state.

## Completed acceptance

- The permanent historical live pointer fixture is reused unchanged as the compiler-proven ownership substrate. At historical_pointer_callee_probe, CFI reconstructs a caller frame whose RBX-owned pointer differs from the current callee state.
- Test-first head 4fc9c8854ed5ae9f470c2a21cf6d0db50048be7a adds a real live CLI workflow: break historical_pointer_callee_probe -> continue -> bt -> frame 1 -> print historical_pointer -> deref historical_pointer -> continue. The dedicated compiler evidence remains green while normal CI fails because mdbg has no frame <index> command.
- Production head ce738caf036415da25f78aa45eb293509b1341a5 adds explicit live inspection-frame selection using the existing EhFrame + build_inspection_frames() path and routes source-value operations through the selected InspectionFrameContext.
- That first production head exposes a real regression: current-frame source lookup incorrectly becomes dependent on the selected-frame routing helper. The regression is not hidden by changing tests.
- Exact corrected head 801c524c63140423d8e5b1807118bea347375619 restores current-frame lookup as the default while retaining explicit historical selection.
- frame <index> always rebuilds the CFI inspection-frame snapshot from the current stop sequence. It does not cache or synthesize a parallel unwind model.
- When a frame is selected, print resolves through the selected InspectionFrameContext; deref uses the existing historical dereference path; aggregate/array/union source-value commands reuse the same selected-frame root routing and context-neutral typed-value operations.
- When no historical frame is selected, source-value commands preserve the pre-existing current-frame behavior.
- Execution control remains tied to the real current machine frame. continue, stepi, source step/next, and finish do not execute against historical state.
- Historical inspection selection is invalidated on every new reported stop, source-motion attempt, register mutation, memory mutation, process selection, and thread selection.
- The fixture/API regression continues to prove stale historical frames are rejected after execution advances; the CLI selection therefore cannot silently fall back to current registers after a new stop.
- Real mdbg proves the full historical CLI flow and exact one-hop pointer result while the existing direct API independently proves the same CFI-owned caller value.
- Exact candidate 801c524c63140423d8e5b1807118bea347375619 is based directly on main 1d0dfab4e46496a95cd897a086abda2a73196730, ahead by three commits and behind by zero, and passes full GCC / Clang-large normal CI plus both dedicated GCC / Clang compiler-evidence lanes.

Phase 81 is sealed here. Additional historical pointer types, alternate caller depths, or routing more already-supported value kinds through the same selected-frame helper are not new milestones.

## Phase 82 promotion — live historical-frame scoped local discovery

The next meaningful gap is discoverability rather than another value shape. Immutable core inspection already has bounded scoped locals discovery, but live mdbg still requires the user to know a variable name in advance. After Phase 81, a selected historical live frame can materialize known names but cannot enumerate the compiler-owned names visible in that frame.

The first coherent Phase 82 slice must:

1. Reuse the existing Phase-81 historical live pointer fixture and a genuine selected caller frame. Do not add a new typed-value fixture solely to populate a list.
2. Introduce one bounded live discover_local_values-style API for Debugger + ElfFile + InspectionFrameContext that reuses the canonical DWARF lexical/subprogram/range/name-resolution machinery. Do not copy the snapshot-only discovery implementation into a second parser.
3. Discovery must remain name/kind only. It must not eagerly evaluate locations, read memory/register values, dereference pointers, or recursively expand objects.
4. Expose locals in live mdbg. With no selected frame it must enumerate the current stopped frame; after frame 1 it must enumerate the historical caller-owned catalogue from that exact InspectionFrameContext.
5. Prove at minimum: frame 1 -> locals contains historical_pointer; a callee-only name is absent from the selected caller catalogue; clearing/invalidation after execution or process/thread ownership change prevents stale catalogue reuse.
6. Preserve the same bounded catalogue size, deterministic ordering, lexical shadowing, abstract-origin naming, and equal-depth ambiguity failure rules already established by immutable snapshot discovery where shared machinery applies.
7. Keep value printing separate from discovery. locals must not become an eager print-all command.
8. Keep arbitrary expression lookup, recursive object enumeration, mutation, synthetic caller state, and speculative DWARF scope expansion out of scope.
9. Require full GCC / Clang-large CI plus independent compiler/CFI evidence and a real live CLI regression on the exact candidate before integration.

This promotion makes explicit live frame selection practically inspectable without broadening the typed-value evaluator or weakening stop-generation ownership.
