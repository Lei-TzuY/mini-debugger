# Phase 13 — Immutable Crash-Cause Metadata

Status: complete for the current bounded Linux x86-64 core-forensics milestone.

## P13-A — Kernel-recorded crash cause

Complete.

The accepted slice is driven by a genuine Linux core produced by a deterministic null-write `SIGSEGV`, not by a synthetic note-only fixture.

- `CoreSnapshot` recognizes only the demonstrated `CORE/NT_SIGINFO` surface and requires the exact native x86-64 `siginfo_t` descriptor size.
- The immutable `CoreCrashInfo` record preserves the kernel-recorded signal number and signal code, plus `si_addr` for bounded fault-class signals where that field is meaningful.
- Crash signal identity is cross-checked against the crash-owner `NT_PRSTATUS`; contradictory core evidence fails closed regardless of note ordering.
- Duplicate or malformed `NT_SIGINFO` records fail closed. A core with no `NT_SIGINFO` remains valid and exposes no fabricated crash metadata while preserving its existing `NT_PRSTATUS` signal evidence.
- `CoreInspectionSession` exposes the same immutable record without changing thread/frame/source/memory authority.
- `mdbg-core crash` renders the bounded kernel evidence and remains deterministic when the optional note is absent.
- Integration mutates a genuine kernel core to prove contradictory signal identity, malformed descriptor size, and optional-note absence behavior.

This milestone deliberately does not enumerate arbitrary ELF core notes, decode signal-specific unions beyond demonstrated fault-address ownership, infer source-level causes, or synthesize missing crash metadata.

## Phase 14 promotion — immutable process identity metadata

The next architectural hypothesis is kernel-recorded process identity from a genuine Linux core. The first slice must begin with executable evidence that the controlled core fixture actually contains a bounded `CORE/NT_PRPSINFO` record; only then may production parse the demonstrated Linux x86-64 layout.

A qualifying Phase 14 slice should preserve immutable process identity such as the recorded PID/parent relationship and bounded process/command text, expose it through the read-only core session/CLI, reject malformed or contradictory evidence where an existing core authority permits a cross-check, and remain inert when the optional note is absent.

Do not build a generic note browser, scrape host `/proc` state after the crash, or infer process identity from `NT_FILE` paths. Kernel-recorded core evidence remains the authority.

## Selection rule

Choose the highest-value core-forensics capability only after a genuine artifact demonstrates the missing evidence and the current implementation cannot expose it. Once a bounded note family is fully covered by one coherent real-core workflow, seal it rather than farming note variants.
