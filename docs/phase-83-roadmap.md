# Phase 83 Roadmap — live inline-context discovery and source-scope selection

Status: complete for the current Linux x86-64 live optimized-source-scope milestone.

Phase 83 makes compiler-produced inline source scopes first-class in live mdbg without fabricating an inline machine frame. One validated live physical inspection frame now owns a bounded active inline call chain, explicit inline source-scope selection, and inline-scoped local discovery.

## Completed acceptance

- The permanent optimized `inline_core_fixture` is reused unchanged. No synthetic inline DIE, hand-authored location expression, or convenience-only fixture is introduced.
- Test-first head `edff618078e89bf91a3fd27bc4d959be683bc53c` leaves dedicated compiler evidence green and fails both normal compiler lanes only because live mdbg has no `inline` command or live inline-scope routing.
- The existing bounded inline DIE/range/call-site parser remains authoritative. Shared ownership-neutral helpers now accept module identity + virtual PC and serve both immutable core and live inspection wrappers; no second live inline parser or scope walker is added.
- Live `discover_inline_call_chain(Debugger, ElfFile, InspectionFrameContext)` validates process/TID/stop/frame ownership, maps the physical frame PC through the owning module load bias, and returns the same bounded outer-to-inner `InlineCallsiteContext` model used by core inspection.
- Live `discover_inline_local_values(..., inline_die_offset)` reuses the canonical inline lexical catalogue machinery, including active-range checks, physical-subprogram ownership, abstract-origin naming, deepest-shadow selection, equal-depth ambiguity rejection, and the 64-name bound.
- Live mdbg now exposes:
  - `inline` to list active inline contexts;
  - `inline <index>` to select one source/DIE scope;
  - `inline physical` to clear inline selection while retaining the physical inspection frame;
  - `locals` routed through the selected inline DIE when one is active.
- Inline selection is source ownership only. It does not create a second register file, CFA, stack pointer, unwind frame, or synthetic machine state.
- Stop changes, source-motion attempts, physical frame selection, process/thread selection, register mutation, and memory mutation all invalidate selected inline ownership.
- API/CLI evidence proves exactly two active contexts `inline_outer -> inline_inner`, correct outer/inner lexical separation, shadow ownership, `inline_pointer` visibility only in the inner scope, and restoration of `physical_only` after `inline physical`.
- Exact implementation head `b447e83cbdf3350cb50a9c3175e3a7e0dbe3f3a0` is based directly on main `288268a04687331d75f0b38ecb938e63471dca3e`, ahead two / behind zero, and passes full GCC / Clang-large normal CI plus both GCC / Clang dedicated compiler/core/session/CLI evidence lanes.

Phase 83 is sealed here. More inline depths, extra catalogue entries, alternate call-site formatting, or eager print-all behavior are not new milestones.

## Phase 84 promotion — compiler-proven live selected-inline typed scalar ownership

The next meaningful frontier is value materialization inside the selected live inline source scope. Discovery and selection are now first-class, while typed `print` still resolves only against the physical inspection frame.

The first coherent Phase 84 slice must:

1. Start from the existing optimized live inline artifact and independently prove one selected-inline binding whose active location can be evaluated exclusively from machine state genuinely owned by the selected physical `InspectionFrameContext`.
2. Reuse the canonical inline DIE ownership, root type resolution, and live scalar evaluator; do not create a live-inline-only type parser or second register/memory reader.
3. Prefer an already-supported exact register or bounded frame-base form emitted by both permanent compiler lanes. If GCC and Clang produce different proven forms, add only those exact forms rather than a generic DWARF VM.
4. Add a live selected-inline inspection API that takes `Debugger + ElfFile + InspectionFrameContext + inline_die_offset + name` and validates inline/physical ownership before evaluating the binding.
5. Route live mdbg `print <name>` through the selected inline scope only when that scope is active. Physical `print` semantics must remain unchanged after `inline physical`.
6. Preserve exact value kind/width/signedness/type metadata and source physical-frame ownership. Selected inline scope must never borrow current registers when a historical physical frame is selected unless the `InspectionFrameContext` explicitly owns those registers.
7. Prove lexical shadow correctness: an inner selected binding must not silently resolve to the physical or outer binding of the same name.
8. Invalidate typed inline ownership on every ownership-domain transition already covered by Phase 83.
9. Keep arbitrary expressions, speculative optimizer reconstruction, pointer traversal, aggregate traversal, mutation, generic location lists/VM, and unsupported historical register borrowing out of scope for this first typed slice.
10. Require full GCC / Clang-large normal CI plus independent compiler evidence and a real live mdbg regression on the exact candidate before integration.

This promotion turns live inline scope selection into real compiler-owned value inspection without weakening the physical-frame ownership model.
