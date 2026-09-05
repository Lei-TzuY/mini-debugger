# Phase 4 roadmap

Phase 4 starts after the Phase 3 process-domain registry removes the remaining one-image/one-process-topology assumption. The next architectural objective is to make source-level debug data executable: the debugger should be able to identify a variable in the current source scope, evaluate the compiler-produced DWARF location that owns its value, read that value from the selected process/TID, and present it without bypassing the existing module, register, memory, CFI, or process-domain ownership models.

## Priority 0: bounded local integer value inspection — complete

The first source-value slice is executable under the permanent GCC and Clang-large gates:

- the existing DWARF4 C fixture now contains a noinline `inspect_local_value` function whose `uint64_t local_value` is derived from a volatile runtime seed and remains live at the exported `local_value_probe`;
- `inspect_local_integer` consumes the owning module's real `.debug_info`, `.debug_abbrev`, and `.debug_str`, with DWARF32/version-4/8-byte-address bounds and explicit rejection for forms outside the compiler-proven surface;
- the selected runtime PC is routed through the existing module resolver and ELF load bias before DIE lookup, so debug information from unrelated modules is never searched as if address spaces were interchangeable;
- the bounded DIE walk resolves the active subprogram, one direct-child local by name, its typedef chain, and a 1–8 byte signed/unsigned base integer type; missing, ambiguous, nested-scope, malformed, or unsupported structures fail explicitly;
- compiler-produced location evaluation is deliberately narrow: the local must use `DW_OP_fbreg`; GCC's `DW_AT_frame_base = DW_OP_call_frame_cfa` is evaluated through the existing `.eh_frame` CFI cursor, while Clang's `DW_OP_reg6 (rbp)` uses the selected thread's register state;
- the final value read goes through `Debugger::read_memory`, preserving selected process/TID ownership instead of introducing a second ptrace path;
- the CLI exposes `print <name>` and renders the resolved signed or unsigned integer according to the recovered base-type width;
- direct API and real `mdbg` subprocess integration stop at `local_value_probe`, observe `0x1020304050607080`, reject a missing local explicitly, then continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

The supported form/opcode set remains intentionally bounded to what those real compiler fixtures emitted; Priority 0 does not claim a general DIE, expression, location-list, aggregate, or optimized-variable engine.

## Priority 1: register-resident formal parameters — complete

The second source-value slice extends the same evaluator to a real optimized formal parameter without introducing a parallel value engine:

- a dedicated `-O1 -g -gdwarf-4` fixture derives a `uint64_t parameter` from a volatile runtime seed and keeps it live in the System V x86-64 first-argument register at the exported `formal_parameter_probe`;
- test-first PIE/non-PIE integration proved the Priority 0 evaluator rejected that value because it only searched direct-child `DW_TAG_variable` DIEs;
- name resolution now also accepts an unambiguous direct-child `DW_TAG_formal_parameter`, while nested lexical locals, ambiguous names, unsupported types, and missing values continue to fail explicitly;
- the only new register-location expression accepted is the compiler-proven `DW_OP_reg5 (rdi)`, read from the existing selected-TID register snapshot and truncated through the same 1–8 byte integer type metadata used by Priority 0;
- initial compiler evidence corrected the original sequencing assumption: normal optimized GCC 13.3 and Clang 18.1 encode the formal parameter's `DW_AT_location` as a DWARF4 `.debug_loc` section offset rather than an inline expression, even though the active PC range itself evaluates to exactly `DW_OP_reg5`;
- to support that real output without claiming general location-list evaluation, the evaluator now has a bounded formal-parameter-only DWARF4 location selector: it honors the compilation unit's `DW_AT_low_pc` as the initial base address, honors explicit base-address-selection entries, selects the range containing the current virtual PC, and returns only that range's expression;
- ordinary optimized `DW_TAG_variable` location lists remain explicitly rejected, and `DW_OP_GNU_entry_value`, `DW_OP_stack_value`, other register numbers, aggregate values, and broader expression evaluation remain outside this milestone;
- direct API and real `mdbg` subprocess integration observe `parameter = 1161981756646125696`, reject a missing parameter explicitly, and continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

The bounded `.debug_loc` selector here exists only because both permanent compiler lanes required it for the formal-parameter workflow. It is not a claim that optimized-local location-list ownership is complete.

## Priority 2: optimized local location-list ownership — current frontier

The same optimized compiler evidence now leaves one distinct executable gap: ordinary local variables can use `DW_FORM_sec_offset` location lists whose active ranges contain expressions more complex than the Priority 1 formal-parameter `DW_OP_reg5` case. GCC may emit GNU location views and stack-value expressions; Clang may move a value between argument/result registers or use entry-value expressions as the PC advances.

Current Priority 2 acceptance:

- add or reuse one deterministic optimized local whose non-constant runtime value is available at a probe through a compiler-produced `.debug_loc` range, and prove the current evaluator rejects that ordinary local before adding support;
- reuse the Priority 1 current-PC range selector rather than creating a second location-list parser, while preserving compilation-unit/base-address semantics and explicit malformed-range rejection;
- implement only the smallest compiler-proven expression needed by that concrete fixture and CI lane; do not enumerate `DW_OP_*` broadly;
- preserve selected process/TID register and memory ownership, existing integer type resolution, module identity, explicit missing/out-of-range failure behavior, and `print <name>` presentation;
- prove PIE/non-PIE and GCC/Clang-large end to end, including a clean exit and a PC position where the value is deliberately unavailable if the compiler output provides such a range boundary;
- do not expand into lexical shadowing, pointers, aggregates, arbitrary stack-machine evaluation, or DWARF5 loclists unless separate executable evidence requires them.

## Later priorities

After Priority 2, lexical shadowing, pointer/aggregate type presentation, richer expression evaluation, and DWARF5 location-list ownership remain candidates. They must continue to be ordered by concrete compiler-produced failures rather than surface-area breadth.

## Selection rule

Priority 2 is the current frontier. Select the smallest real optimized-local workflow whose `.debug_loc` entry is already PC-selectable by the Priority 1 machinery but whose expression is still unsupported, prove that failure first, and implement only the expression/location ownership required to make that workflow correct under the permanent GCC and Clang-large gates.
