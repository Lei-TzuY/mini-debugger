# Phase 7 roadmap

Phase 7 begins after Phase 6 separates live execution ownership from caller-frame inspection ownership. Its next architectural hypothesis is **immutable post-mortem inspection**: source/symbol/unwind/value inspection should be able to operate on an owned ELF64 core snapshot without pretending that snapshot has a resumable ptrace task.

A core snapshot is not a `Process`. It cannot continue, single-step, receive a signal, mutate registers/memory, own software-breakpoint displacement, or program hardware debug registers. Its registers and memory are immutable evidence captured at one crash state. Phase 7 must preserve that distinction rather than adding a fake PID/TID behind the existing live debugger API.

## Priority 0: bounded ELF64 core snapshot ownership — current frontier

The first slice must establish a real post-mortem data plane before adding source-level commands.

Acceptance criteria:

- start from a deterministic x86-64 Linux core file produced by a real crashing fixture in CI or an equivalently controlled kernel-generated artifact; do not hand-author note bytes as the primary evidence;
- parse only the required ELF64 little-endian x86-64 core surface, validating `ET_CORE`, program-header bounds, note bounds, alignment, and malformed/truncated input explicitly;
- recover at least one concrete crashed-thread register snapshot from Linux `NT_PRSTATUS`, including RIP/RSP/RBP and the general-purpose register state required by the first inspection workflow;
- expose bounded immutable reads from owned `PT_LOAD` file-backed snapshot bytes, with deterministic failure for unmapped, truncated, overflowed, or zero-fill-only ranges that are not represented by captured bytes;
- retain enough file-backed mapping/module identity from core metadata or a separately proven mapping source to associate the crashed RIP with the correct executable/module; do not guess a module from the host process;
- prove the snapshot is non-executable: resume, step, signal delivery, register/memory mutation, breakpoint/watchpoint operations, and live process-domain selection must be unavailable by construction rather than runtime no-ops;
- add focused parser tests for malformed boundaries plus an end-to-end PIE/non-PIE post-mortem integration that reads the real crash RIP and owned memory from the generated core artifact.

Priority 0 is intentionally not a full GDB core-file implementation. Multi-thread note selection, shared-library reconstruction, CFI/source-value reuse, signal metadata presentation, and CLI productization belong to later slices only after the immutable ownership boundary is executable and verified.

## Selection rule

Choose the smallest real core artifact that proves immutable register + memory ownership without depending on host ptrace state. Do not start with generic ELF-note enumeration, every Linux note type, or UI commands before the snapshot data plane exists.
