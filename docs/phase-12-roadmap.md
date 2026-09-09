# Phase 12 roadmap

Phase 12 begins after the sealed Phase 11 provenance-aware snapshot-value milestone. Phase 11 proved that higher-level immutable value inspection can consume captured or explicitly reconstructed file-backed bytes without changing the authority model. Phase 12 moves to a different product boundary: **post-mortem source-context presentation**.

The core session already owns immutable thread/frame selection, module-qualified symbol and source coordinates, snapshot value inspection and provenance-aware memory reads. Source text shown to a user is not core evidence. It is a local presentation artifact selected from the recorded debug identity through an explicit bounded path policy.

## P12-A: selected-frame source context — complete

Completed executable capability:

- `mdbg-core list` / `l` renders source context for the currently selected immutable snapshot frame rather than stopping at `module!file:line` coordinates;
- the command resolves the selected frame through the existing immutable `CoreInspectionSession::find_source()` path, preserving recorded module/debug ownership before any host source file is consulted;
- local source-file lookup reuses the existing component-aware `SourcePathResolver` policy. It checks only the recorded source candidate and its bounded module-relative fallback; it does not recursively scan the filesystem or infer a same-named source tree;
- source presentation is bounded to four lines before and after the recorded row, marks the current row explicitly, streams the host file rather than loading it without bound, and reports deterministic non-fatal unavailability when the file cannot be opened or the recorded row lies outside it;
- genuine Linux kernel-core integration selects the recovered `inspect_entry_parameter` caller frame and requires real fixture source text to be rendered under both permanent GCC and Clang-large CI lanes;
- existing immutable thread selection, backtrace, source-value inspection, separate-debug-file identity, omitted-memory artifact reconstruction and rejection of live execution commands remain unchanged.

P12-A does not copy source files into the core, label host source text as snapshot provenance, mutate snapshot state, or create a second DWARF/source resolver. Recorded `module!file:line` remains the debug identity; source text is presentation only.

## P12-B: explicit relocated-source substitution — complete

Completed executable capability:

- `mdbg-core` accepts repeatable `--substitute-source-path <recorded-prefix> <local-prefix>` rules and injects them into the same `SourcePathResolver` already used by immutable `list` rendering;
- the mapping is host-presentation policy only. It does not rewrite the core, recorded module path, DWARF source identity, separate-debug ownership, frame selection, source-value evaluation or snapshot-memory provenance;
- source substitution therefore inherits the existing component-aware and most-specific-prefix resolver semantics rather than introducing filesystem scans, basename guessing or a second path engine;
- integration first generates a genuine Linux kernel core, then physically moves the real `formal_parameter_fixture.c` source out of its recorded source-tree location. With no mapping, the same core preserves its module-qualified caller/frame identity but deterministically reports `source unavailable` and cannot render the source text;
- applying one explicit recorded-prefix to relocated-prefix mapping to that same immutable core restores the real `inspect_entry_parameter` source excerpt while leaving module/frame ownership unchanged;
- the relocation harness restores the source tree on every normal and exceptional path, and the complete workflow is proven under both permanent GCC and Clang-large lanes.

P12-B is not workspace discovery. A missing source tree remains missing unless the user supplies an explicit mapping; host source files never become snapshot evidence.

## Phase 12 bounded milestone — complete

P12-A and P12-B establish the complete bounded post-mortem source-presentation contract required by the demonstrated workflows: immutable source identity comes from core/debug evidence, while optional host text is resolved through an explicit bounded local policy that also survives a demonstrated relocated source tree.

Adding alternate line radii, basename search, automatic workspace discovery, extra path-remap syntaxes or source-layout variants without a separate failing workflow would be presentation farming, so Phase 12 is sealed here.

## Phase 13 promotion: immutable crash-cause metadata — next architectural hypothesis

The core session can now answer where the crashed thread was executing, recover callers, inspect source values and memory provenance, and render source context even when the host source tree has been explicitly relocated. A distinct post-mortem evidence gap remains below those presentation layers: the ELF core parser currently consumes thread `NT_PRSTATUS` and module `NT_FILE` notes but does not expose the kernel-recorded `NT_SIGINFO` crash metadata.

Phase 13 should begin with a genuine deterministic fault whose Linux core contains `NT_SIGINFO`. The first slice must parse only the demonstrated x86-64 Linux note layout, bind it to the immutable crash record, and expose at least the recorded signal number/code and fault address (`si_addr`) through the read-only core session/CLI. The implementation must cross-check signal identity rather than silently replacing `NT_PRSTATUS` evidence, reject malformed or contradictory note data, and remain inert for core files where the optional note is absent.

Do not turn Phase 13 into generic ELF-note enumeration. Additional signal fields or note types require a concrete diagnostic workflow that cannot be satisfied by the bounded crash-cause record.

## Selection rule

Choose the smallest real post-mortem workflow whose answer is still missing after the sealed source-presentation milestone. Preserve immutable core authority and explicit host-artifact boundaries; do not return to source-path or presentation variants without a separately demonstrated failure.
