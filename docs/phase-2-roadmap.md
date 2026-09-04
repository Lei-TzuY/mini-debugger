# Phase 2 roadmap

Phase 2 starts only after the `v0.1.0` stabilization gates are satisfied. The goal is to add debugger capabilities that materially change what the project can demonstrate, not to maximize commit count or instruction-encoding coverage.

## Priority 0: decoder architecture gate — complete

The bounded source-`next` near-call recognizer has been extracted from `source_step.cpp` into the explicit `x86/call_decoder` component with direct unit tests that do not launch a tracee. Existing supported call classes retain their behavior, unsupported/truncated inputs remain explicit fallbacks, and the debugger-facing byte reader stays lazy instead of speculatively reading full instructions.

This closes the architecture gate for future decoder work without turning the project into a general-purpose disassembler. Evidence and scope boundaries are recorded in `docs/phase-2-p0-decoder.md`.

## Priority 1: dynamic-loader events and deferred breakpoints — current frontier

Teach the debugger to handle breakpoints requested for symbols/source locations in modules that are not loaded yet.

Acceptance criteria:

- a breakpoint request can remain pending when its target shared object is absent;
- loader activity causes a bounded re-resolution attempt;
- a uniquely resolved target installs through the existing managed breakpoint state machine;
- ambiguity remains an explicit error;
- unload/reload behavior has deterministic ownership semantics and regression coverage.

Why first: this closes a real usability gap in the existing module-aware breakpoint model instead of adding another presentation-only feature.

## Priority 2: hardware watchpoints

Add x86-64 debug-register watchpoints without weakening software-breakpoint ownership.

Acceptance criteria:

- bounded support for read/write or write watchpoints with documented alignment/length rules;
- trap classification distinguishes hardware-watchpoint stops from managed `INT3` stops;
- watchpoint state is restored/cleared correctly across detach and teardown;
- deterministic integration tests cover hit, delete, coexistence with software breakpoints, and lifecycle cleanup.

## Priority 3: DWARF 5 line tables

Extend source mapping beyond the intentionally bounded DWARF v4 reader.

Acceptance criteria:

- v4 behavior remains unchanged;
- supported v5 directory/file table forms are parsed with explicit bounds;
- unsupported forms fail explicitly rather than being guessed;
- address-to-source and file:line reverse lookup share the same parsed representation;
- fixtures exercise both main executables and shared objects.

## Priority 4: multi-thread debugging

Move from one traced thread to a deliberate thread model.

Acceptance criteria:

- tracee state is tracked per TID where required;
- stop/resume policy is documented and deterministic;
- software breakpoint ownership remains process-wide while displaced execution is associated with the correct stopped thread;
- thread creation/exit and signal routing have regression coverage;
- detach/teardown leaves no traced thread behind.

This milestone should not be attempted as an incidental extension of the current single-thread state machine.

## Priority 5: broader CFI recovery

Broaden `.eh_frame` support only when driven by real compiler output that currently fails.

Acceptance criteria:

- each added pointer encoding/opcode/register rule is justified by a reproducible fixture;
- malformed/unsupported CFI remains distinguishable from "no applicable CFI";
- arbitrary register values are never invented;
- cross-module unwind behavior remains bounded and deterministic.

## Priority 6: source display and CLI productization

Improve presentation after the underlying execution semantics are stable.

Possible scope:

- source-context display around the current line;
- clearer module-qualified symbol/source rendering;
- CLI help/usage consistency;
- release packaging and examples.

This work should not precede correctness-critical lifecycle, loader, watchpoint, thread, or unwind work when those are active milestones.

## Selection rule

Choose the highest-priority milestone that has a concrete failing scenario or acceptance test and no overlapping active implementation. A milestone may be split into bounded PRs, but every PR must advance the milestone itself; repeated variant-only micro-PRs are not a roadmap strategy.
