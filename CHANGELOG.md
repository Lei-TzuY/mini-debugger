# Changelog

All notable project milestones are recorded here.

## v0.1.0 — first usable debugger milestone

### Added

- x86-64 Linux tracing through `ptrace(2)` with launch and attach workflows.
- Explicit launched-versus-attached lifecycle ownership and safe detach semantics.
- Managed software breakpoints with original-byte ownership, RIP repair, displaced-instruction stepping, and reinsertion.
- General-purpose register inspection, memory reads, and explicit signal suppression/forwarding.
- In-tree ELF64 symbol parsing with PIE and shared-object load-bias resolution.
- Module-aware symbol and DWARF v4 source breakpoints across already loaded ordinary file-backed modules.
- Module-aware source-level `step`, bounded source-level `next`, and frame-oriented `finish`.
- Bounded `.eh_frame` CFI unwinding with validated frame-pointer fallback.
- Cross-module backtrace symbolization and DWARF v4 source presentation.
- Deterministic PIE/non-PIE/stripped, omitted-frame-pointer, and shared-library regression fixtures.

### Stabilization policy

`v0.1.0` closes the unconditional feature-generation phase. Further work follows `docs/phase-2-roadmap.md`; broader x86 `next` coverage first requires an explicit decoder abstraction with direct tests rather than encoding-by-encoding micro-features.

### Known limits

- Linux x86-64 only.
- One traced process/thread.
- Source mapping is limited to the supported DWARF v4 `.debug_line` subset.
- `.eh_frame` support intentionally covers a bounded common CFI subset.
- Breakpoints in not-yet-loaded modules are not deferred.
- Hardware watchpoints, DWARF5, multi-thread debugging, dynamic-loader events, and broader instruction decoding are not implemented yet.
