# mini-debugger v0.1.0

`v0.1.0` is the first usable milestone of `mini-debugger`: a compact x86-64 Linux debugger built directly on `ptrace(2)` with small in-tree ELF64, DWARF line-table, and `.eh_frame` readers.

## What this release can do

- launch a tracee or attach to an existing PID with explicit ownership-aware teardown;
- inspect x86-64 general-purpose registers and tracee memory;
- manage software breakpoints safely across trap classification, RIP repair, displaced execution, reinsertion, deletion, and detach;
- resolve ELF symbols for PIE executables and loaded shared objects;
- set symbol and DWARF v4 source breakpoints across currently loaded ordinary file-backed modules;
- perform bounded module-aware source `step` and source `next`;
- perform `finish` and bounded backtraces through supported `.eh_frame` CFI, with validated frame-pointer fallback;
- render cross-module symbols and supported DWARF v4 source locations in backtraces and address-oriented source presentation.

## Scope boundary

This is intentionally not a general-purpose GDB replacement. The supported platform is Linux x86-64, the process model is currently single-threaded, DWARF source support is limited to the implemented v4 `.debug_line` subset, and CFI support is deliberately bounded. Breakpoints cannot yet wait for future shared-library loads. Hardware watchpoints, DWARF5, dynamic-loader events/deferred breakpoints, multi-thread debugging, broader CFI recovery, and general instruction decoding remain Phase 2 work.

The existing source-`next` implementation recognizes a bounded set of direct and indirect x86-64 near-call forms. `v0.1.0` freezes further encoding-by-encoding expansion: broader coverage must first pass the decoder-architecture gate documented in `docs/phase-2-roadmap.md`.

## Verification

The release candidate is required to have an exact-main successful GitHub Actions build/test run, no open implementation PRs or issues, and no release-critical TODO/FIXME in the core lifecycle, breakpoint, source-motion, symbol/source-resolution, or unwind paths.

See `docs/v0.1.0-checkpoint.md` for the release checklist and `docs/phase-2-roadmap.md` for post-release priorities.
