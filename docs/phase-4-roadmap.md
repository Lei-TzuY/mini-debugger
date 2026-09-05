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
- to support that real output without claiming general location-list evaluation, the evaluator gained a bounded DWARF4 location selector: it honors the compilation unit's `DW_AT_low_pc` as the initial base address, honors explicit base-address-selection entries, selects the range containing the current virtual PC, and returns only that range's expression;
- ordinary optimized `DW_TAG_variable` location lists remained explicitly rejected at this milestone, and `DW_OP_GNU_entry_value`, `DW_OP_stack_value`, other register numbers, aggregate values, and broader expression evaluation remained outside scope;
- direct API and real `mdbg` subprocess integration observe `parameter = 1161981756646125696`, reject a missing parameter explicitly, and continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

## Priority 2: optimized local location-list ownership — complete

The third source-value slice extends that same current-PC ownership model to one real optimized ordinary local:

- the existing `-O1 -g -gdwarf-4` fixture now derives a non-constant `uint64_t optimized_local` from the volatile runtime seed, constrains the same local through RDI and then through RAX, and stops at the exported `optimized_local_probe` while the value is live;
- test-first GCC 13.3 and Clang 18.1 evidence was identical: both PIE and non-PIE builds reached the existing `optimized local-variable location lists are outside this milestone` rejection while every other integration test remained green, proving the ordinary `DW_TAG_variable` really owns a `DW_FORM_sec_offset` `.debug_loc` list rather than an inline fixed location;
- ordinary locals now reuse the Priority 1 DWARF4 current-PC selector directly, including compilation-unit initial-base semantics, explicit base-address-selection entries, range validation, and deterministic no-location/out-of-range failures; no second location-list parser was introduced;
- the only new expression opcode is the compiler-proven `DW_OP_reg0 (rax)` used by the active probe range; it is read from the selected TID's existing register snapshot and truncated through the same bounded integer type metadata as the earlier `DW_OP_reg5` path;
- `DW_OP_reg5` and `DW_OP_fbreg` retain their existing behavior, while other register numbers, compound stack-machine expressions, entry-value/stack-value operations, pointer/aggregate values, and DWARF5 loclists remain rejected;
- direct API and real `mdbg` subprocess integration observe `optimized_local = 2178649820992642800`, reject a missing optimized local explicitly, preserve module ownership, and continue to clean exit in PIE and non-PIE under both permanent compiler lanes.

Priority 2 therefore closes the first bounded optimized-local ownership gap without turning the source-value evaluator into a speculative general DWARF VM.

## Priority 3: lexical-scope local ownership — current frontier

The next architectural gap is name ownership rather than another register opcode. The evaluator currently accepts only unambiguous variables that are direct children of the active subprogram and explicitly rejects nested lexical-scope locals. That bound becomes incorrect once a real source block owns a live local or shadows an outer name at the stopped PC.

Current Priority 3 acceptance:

- add one deterministic compiler-produced nested lexical block, preferably with an outer and inner integer sharing a name, and first prove the current evaluator reaches its explicit nested-scope rejection or ambiguous-name boundary at the inner probe;
- parse and honor only the lexical-scope DIE/range structure emitted by that concrete GCC/Clang fixture so lookup chooses the innermost scope that actually contains the current virtual PC;
- keep value evaluation delegated to the already proven location machinery (`DW_OP_fbreg`, `DW_OP_reg0`, `DW_OP_reg5`, and DWARF4 `.debug_loc` selection) unless the fixture independently proves a new expression is required;
- preserve selected process/TID ownership, module identity, integer type resolution, explicit missing/out-of-scope failure behavior, and `print <name>` presentation;
- prove outer-versus-inner ownership at distinct PCs, PIE/non-PIE behavior, GCC/Clang-large compatibility, and clean exit;
- do not expand into pointers, aggregates, arbitrary expression evaluation, inlined-subroutine scope ownership, or DWARF5 loclists without separate executable evidence.

## Later priorities

After Priority 3, pointer/aggregate type presentation, richer expression evaluation, inlined-scope ownership, and DWARF5 location-list ownership remain candidates. They must continue to be ordered by concrete compiler-produced failures rather than surface-area breadth.

## Selection rule

Priority 3 is the current frontier. Select the smallest real nested/shadowed-local workflow that the current direct-child name ownership model rejects, prove that failure first under the permanent GCC and Clang-large gates, and implement only the lexical DIE/range ownership needed to make that workflow correct. Do not use Priority 3 as a reason to broaden location-expression support unless the chosen compiler output independently requires it.
