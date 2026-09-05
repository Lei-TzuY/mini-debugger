# Phase 4 roadmap

Phase 4 starts after the Phase 3 process-domain registry removes the remaining one-image/one-process-topology assumption. The next architectural objective is to make source-level debug data executable: the debugger should be able to identify a variable in the current source scope, evaluate the compiler-produced DWARF location that owns its value, read that value from the selected process/TID, and present it without bypassing the existing module, register, memory, CFI, or process-domain ownership models.

## Priority 0: bounded local integer value inspection — current frontier

The first slice must end in a real value observed from a real compiler-generated debug record. A parser-only `.debug_info` API, a hand-authored DIE fixture, or a CLI command that merely accepts a variable name does not satisfy the milestone.

Acceptance:

- add a deterministic C/C++ fixture with one noinline function containing a named fixed-width integer local initialized to a known non-constant runtime value, then stop at a symbol/source probe while that local is live;
- consume the owning module's compiler-generated `.debug_info`, `.debug_abbrev`, and referenced string data rather than introducing synthetic DIE bytes; start with the smallest GCC/Clang DWARF surface actually emitted by the fixture and reject unsupported unit/form/tag structures explicitly;
- locate the current subprogram/lexical scope from the selected runtime PC using the module's ELF load bias, then resolve one unambiguous local-variable DIE by name without scanning unrelated modules as if their address spaces were interchangeable;
- evaluate only the compiler-proven location/frame-base operations required by the fixture. If GCC and Clang emit different location forms, each additional form must be justified by an executable failing lane rather than pre-implemented from the DWARF opcode table;
- route register and memory reads through the selected debugger process/TID. Variable inspection must therefore remain correct after thread selection and must not introduce a second raw-ptrace path;
- recover any required frame/CFA base through the existing bounded unwind/CFI ownership model rather than assuming RBP is always the source frame base;
- resolve the variable's base integer type sufficiently to know byte width and signedness, read exactly that bounded value, and expose one user-visible command such as `print <name>` that renders the observed value;
- prove the end-to-end workflow in PIE and non-PIE under GCC and Clang-large: stop in the fixture, print the named local, compare against the known runtime value, then continue to clean exit;
- malformed/unsupported debug info, unavailable location state, ambiguous names, unreadable memory, and out-of-scope variables must fail explicitly without mutating the inferior.

Do not expand this first slice into a general C/C++ expression evaluator, arbitrary composite pretty-printer, optimized location-list engine, or complete DWARF DIE library. The point is to establish one real source-value path that later milestones can extend from evidence.

## Later priorities

After Priority 0 is executable and stable, the next likely value-layer milestones are parameter/register-resident locations, lexical shadowing and location lists, pointer/aggregate type presentation, and finally a bounded expression surface. Their order is intentionally not frozen until compiler-generated fixtures expose concrete gaps.

## Selection rule

Priority 0 is the current frontier. Select the smallest real compiler-produced local-variable workflow that fails because source-level value semantics are absent, prove that failure first, and implement only the DWARF forms/location operations necessary to make that workflow correct under the existing GCC and Clang-large CI gates. Do not replace executable evidence with a broad DWARF opcode checklist.