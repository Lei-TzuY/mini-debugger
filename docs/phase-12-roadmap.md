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

## Phase 12 current frontier

Do not add source-path substitution variants, automatic workspace discovery or additional presentation commands without a concrete failing post-mortem workflow. The next slice must begin with a genuine core whose selected frame has valid recorded source identity but whose source context remains unusable for a distinct reason (for example, an explicitly demonstrated relocated source tree). If such a case is proven, extend the same bounded presentation policy rather than scanning the filesystem or weakening module/debug ownership.

## Selection rule

Choose the smallest real immutable-debugging workflow whose source presentation is still missing after P12-A. Preserve core read-only semantics and the distinction between recorded debug identity, snapshot/artifact evidence and local host presentation paths. Do not farm nearby line-radius values, aliases, source-file layouts or path-remap combinations without a separately demonstrated failure.
