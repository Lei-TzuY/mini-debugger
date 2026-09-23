# Phase 82 Roadmap — live historical-frame scoped local discovery

Status: complete for the current Linux x86-64 live multi-frame discoverability milestone.

Phase 82 closes the discoverability gap left by explicit historical inspection-frame selection. Live mdbg can now enumerate compiler-owned local and formal-parameter names for either the current stopped frame or an explicitly selected CFI-recovered historical frame without evaluating any value location or reading registers/memory for the listed values.

## Completed acceptance

- The Phase-81 historical pointer fixture is reused unchanged. No new typed-value fixture was added solely to populate a catalogue.
- Test-first head `e008564b6638bbc7160d71145d629e3d9219bff4` adds the real live CLI workflow `locals -> frame 1 -> locals -> print historical_pointer -> deref historical_pointer`.
- On that exact red head, both dedicated GCC/Clang compiler-evidence lanes remain green and 84/86 ordinary CTests pass in both normal compiler lanes. The only failures are the PIE/non-PIE historical pointer integrations because live mdbg has no `locals` command and therefore prints the generic command list.
- The former snapshot-named lexical helper `discover_snapshot_unit()` is renamed to ownership-neutral `discover_scoped_local_catalogue_unit()`. The helper still performs the single canonical bounded DWARF catalogue operation: physical subprogram selection, abstract-origin naming, lexical-scope activity, deepest-shadow selection, equal-depth ambiguity rejection, 64-name cap, and deterministic name ordering.
- Snapshot discovery and live discovery both call that same helper. No second lexical parser, scope walker, symbol catalogue, or location evaluator is introduced.
- The new live `discover_local_values(Debugger, ElfFile, InspectionFrameContext)` validates stopped process/TID/stop-generation/origin-register ownership through the existing inspection-frame validator, maps the selected frame runtime PC through the owning module load bias, and enumerates only name/kind metadata.
- Discovery remains intentionally separate from materialization. It does not inspect `DW_AT_location`, evaluate a location expression, read a value register, read value memory, dereference a pointer, or expand an object.
- Live mdbg now exposes `locals`. With no explicit historical selection it uses `source_inspection_frame()` and lists the current stopped-frame catalogue. After `frame 1` it lists the catalogue for that exact historical `InspectionFrameContext`.
- API and real CLI evidence prove the current callee catalogue contains formal parameter `input`, historical frame 1 contains formal parameter `historical_pointer`, and the historical catalogue does not leak the callee-only `input` binding.
- Existing `print historical_pointer` and `deref historical_pointer` remain independently functional immediately after historical discovery, proving catalogue selection did not mutate or replace source-value ownership.
- Advancing execution invalidates the old `InspectionFrameContext`; both value inspection and catalogue discovery reject the stale frame rather than silently falling back to current state.
- Exact production head `4b2d64a2bbc7e886140ba087f854106ccb89d461` is based directly on main `1e65e1ed8f6a5227fd86f71301b4f3db23055a48`, ahead two / behind zero, and passes full GCC / Clang-large normal CI plus both dedicated GCC / Clang compiler/core/session/CLI evidence lanes.

Phase 82 is sealed here. Adding more names, more historical depths, eager print-all behavior, or catalogue-only variants is not a new milestone.

## Phase 83 promotion — live inline-context discovery and source-scope selection

The next meaningful ownership frontier is optimized inline source context. Immutable core inspection already has bounded compiler-proven inline-callsite reconstruction, inline-context selection, inline-scoped local catalogues, and typed selected-inline values. Live mdbg has no corresponding inline context API or `inline` command at all, even though it now has explicit physical inspection-frame selection and scoped discovery.

The first coherent Phase 83 slice must:

1. Start from genuine optimized GCC and Clang live artifacts with at least one active compiler-produced `DW_TAG_inlined_subroutine`. Reuse an existing optimized inline fixture if it can be stopped live at the exact evidenced PC; do not synthesize inline DIEs or hand-author a location.
2. Refactor the existing bounded inline-call-chain discovery into an ownership-neutral module + virtual-PC helper usable by immutable snapshot and live `InspectionFrameContext` callers. Do not create a second live inline parser.
3. Add a live API that attaches the discovered source inline chain to one validated current or explicitly selected historical physical inspection frame. Inline contexts remain source/DIE scopes only; they must not fabricate a separate register file, CFA, stack pointer, or unwind frame.
4. Expose live mdbg `inline` to list active inline contexts, `inline <index>` to select one, and `inline physical` to clear inline selection while retaining the physical inspection frame.
5. Route `locals` through the selected inline DIE using the already-canonical inline lexical catalogue machinery. Physical/current/historical frame selection and inline selection must compose without flattening scopes or leaking descendant names.
6. Add typed `print` in the same phase only if independent compiler evidence proves that one selected-inline live binding can be materialized exclusively from machine state genuinely owned by the selected physical frame and already-supported evaluator forms. Otherwise keep the first slice discovery/selection-only and record the evaluator blocker explicitly rather than borrowing unrelated current registers.
7. Invalidate selected inline ownership on every new stop, source-motion attempt, register/memory mutation, process/thread selection, or physical-frame selection change. A stale inline selection must never survive an ownership-domain transition.
8. Preserve the existing eight-context cap, active-range ownership, abstract-origin naming, call-site metadata, deterministic ordering, ambiguity failures, and module identity rules.
9. Keep speculative expression evaluation, synthetic caller machine state, recursive object enumeration, arbitrary inline-depth expansion, and unsupported DWARF range/location forms out of scope.
10. Require full GCC / Clang-large normal CI plus independent compiler evidence and a real live mdbg regression on the exact candidate before integration.

This promotion makes optimized source scopes first-class in the live debugger without weakening the physical-frame ownership model established by Phases 71–82.
