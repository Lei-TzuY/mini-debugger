# Phase 4 roadmap

Phase 4 starts after the Phase 3 process-domain registry removes the remaining one-image/one-process-topology assumption. The next architectural objective is to make source-level debug data executable: the debugger should be able to identify a variable in the current source scope, evaluate the compiler-produced DWARF location that owns its value, read that value from the selected process/TID, and present it without bypassing the existing module, register, memory, CFI, or process-domain ownership models.

## Priority 0: bounded local integer value inspection — complete

The first source-value slice is executable under the permanent GCC and Clang-large gates:

- the existing DWARF4 C fixture contains a noinline `inspect_local_value` function whose `uint64_t local_value` is derived from a volatile runtime seed and remains live at the exported probe;
- `inspect_local_integer` consumes the owning module's real `.debug_info`, `.debug_abbrev`, and `.debug_str`, with DWARF32/version-4/8-byte-address bounds and explicit rejection for forms outside the compiler-proven surface;
- the selected runtime PC is routed through the existing module resolver and ELF load bias before DIE lookup, so debug information from unrelated modules is never searched as if address spaces were interchangeable;
- the bounded DIE walk resolves the active subprogram, one local by name, its typedef chain, and a 1–8 byte signed/unsigned base integer type; missing, ambiguous, malformed, or unsupported structures fail explicitly;
- compiler-produced location evaluation is deliberately narrow: the local must use `DW_OP_fbreg`; GCC's `DW_AT_frame_base = DW_OP_call_frame_cfa` is evaluated through the existing `.eh_frame` CFI cursor, while Clang's `DW_OP_reg6 (rbp)` uses the selected thread's register state;
- the final value read goes through `Debugger::read_memory`, preserving selected process/TID ownership instead of introducing a second ptrace path;
- the CLI exposes `print <name>` and renders the resolved signed or unsigned integer according to the recovered base-type width;
- direct API and real `mdbg` subprocess integration observe the expected runtime value, reject a missing local explicitly, then continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

The supported form/opcode set remains intentionally bounded to what those real compiler fixtures emitted; Priority 0 does not claim a general DIE, expression, location-list, aggregate, or optimized-variable engine.

## Priority 1: register-resident formal parameters — complete

The second source-value slice extends the same evaluator to a real optimized formal parameter without introducing a parallel value engine:

- a dedicated `-O1 -g -gdwarf-4` fixture derives a `uint64_t parameter` from a volatile runtime seed and keeps it live in the System V x86-64 first-argument register at the exported `formal_parameter_probe`;
- test-first PIE/non-PIE integration proved the Priority 0 evaluator rejected that value because it only searched direct-child `DW_TAG_variable` DIEs;
- name resolution also accepts an unambiguous direct-child `DW_TAG_formal_parameter`, while unsupported types and missing values continue to fail explicitly;
- the only new register-location expression accepted is the compiler-proven `DW_OP_reg5 (rdi)`, read from the existing selected-TID register snapshot and truncated through the same 1–8 byte integer type metadata used by Priority 0;
- real optimized GCC 13.3 and Clang 18.1 output places the formal parameter's `DW_AT_location` in a DWARF4 `.debug_loc` section offset; the bounded selector honors the compilation unit's `DW_AT_low_pc`, explicit base-address-selection entries, and the range containing the current virtual PC;
- `DW_OP_GNU_entry_value`, `DW_OP_stack_value`, other register numbers, aggregate values, and broader expression evaluation remain outside scope;
- direct API and real `mdbg` subprocess integration observe the expected parameter value, reject a missing parameter explicitly, and continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

## Priority 2: optimized local location-list ownership — complete

The third source-value slice extends that same current-PC ownership model to one real optimized ordinary local:

- the existing `-O1 -g -gdwarf-4` fixture derives a non-constant `uint64_t optimized_local` from the volatile runtime seed, constrains the same local through RDI and then through RAX, and stops at the exported `optimized_local_probe` while the value is live;
- test-first GCC 13.3 and Clang 18.1 evidence was identical: both PIE and non-PIE builds reached the earlier optimized-local location-list rejection while every other integration test remained green, proving the ordinary `DW_TAG_variable` really owns a `DW_FORM_sec_offset` `.debug_loc` list rather than an inline fixed location;
- ordinary locals reuse the Priority 1 DWARF4 current-PC selector directly, including compilation-unit initial-base semantics, explicit base-address-selection entries, range validation, and deterministic no-location/out-of-range failures; no second location-list parser was introduced;
- the only new expression opcode is the compiler-proven `DW_OP_reg0 (rax)` used by the active probe range; it is read from the selected TID's existing register snapshot and truncated through the same bounded integer type metadata as the earlier `DW_OP_reg5` path;
- `DW_OP_reg5` and `DW_OP_fbreg` retain their existing behavior, while other register numbers, compound stack-machine expressions, entry-value/stack-value operations, pointer/aggregate values, and DWARF5 loclists remain rejected;
- direct API and real `mdbg` subprocess integration observe the expected optimized local, reject a missing optimized local explicitly, preserve module ownership, and continue to clean exit in PIE and non-PIE under both permanent compiler lanes.

Priority 2 therefore closes the first bounded optimized-local ownership gap without turning the source-value evaluator into a speculative general DWARF VM.

## Priority 3: lexical-scope local ownership — complete

The fourth source-value slice fixes name ownership for real nested lexical scopes without broadening the location VM:

- the existing O0 DWARF4 fixture now gives `inspect_local_value` an outer `uint64_t local_value` and an inner lexical block with a shadowing `uint64_t local_value`; both are runtime-derived and deliberately hold different values;
- the first fixture-only run was test-first evidence: GCC 13.3 and Clang 18.1 both configured and built successfully, 40/42 tests remained green, and only `dwarf_line_integration_{pie,nopie}` failed because the evaluator returned the outer value while stopped at the inner `local_value_probe`;
- the DIE parser already preserved parent indexes, so production only adds the compiler-proven `DW_TAG_lexical_block` ownership rule rather than introducing a second DIE model;
- lexical blocks are bounded to the concrete `DW_AT_low_pc` / `DW_AT_high_pc` contiguous range shape emitted by the fixture; a nested value is active only when every lexical ancestor between it and the owning subprogram contains the current virtual PC;
- matching locals are ranked by active lexical depth, so the innermost active scope shadows outer names; same-depth matches remain an explicit ambiguity, unsupported non-lexical nesting is not treated as a lexical scope, and an inactive nested DIE does not hide an outer local;
- value type and location evaluation remain delegated to the already proven integer/type, `DW_OP_fbreg`, register, frame-base, and `.debug_loc` machinery; Priority 3 adds no new expression opcode or location-list format;
- the final fixture exports outer-before, inner, and outer-after probes. Direct API and real `mdbg` subprocess integration stop at all three in one tracee and observe outer → inner → outer values in order, proving both shadowing and ownership restoration after leaving the nested block;
- PIE and non-PIE pass under both GCC and Clang-large, module identity and missing-name failures remain intact, and the tracee exits cleanly after the three managed breakpoint stops.

Priority 3 therefore closes the bounded compiler-produced lexical-shadowing gap. Sibling-block variants, deeper nesting, inlined-subroutine ownership, range lists, and additional expression opcodes are not follow-up micro-PR targets without independent executable evidence.

## Priority 4: bounded pointer scalar inspection — current frontier

The next architectural gap is the value type model rather than name lookup. `inspect_local_integer` and `LocalIntegerValue` currently require a typedef/base-integer chain, so a normal source pointer can have a perfectly supported location while still failing when its `DW_AT_type` resolves to `DW_TAG_pointer_type`.

Current Priority 4 acceptance:

- add one deterministic compiler-produced local pointer whose pointee and pointer value remain live at an exported probe, and first prove the current evaluator fails specifically at the pointer type boundary under GCC and Clang-large;
- extend the existing source-value result/type path rather than adding a parallel raw-memory command: preserve owning module, variable name, selected process/TID, and the already proven location evaluator;
- support only the concrete pointer DIE/type links and pointer-width metadata emitted by that fixture, with explicit rejection for unsupported address sizes, malformed type references, aggregates, references, arrays, or compound value pieces;
- render the pointer scalar deterministically in the CLI and prove the recovered address equals the live runtime object address; dereferencing should be added only if the chosen executable acceptance requires it rather than bundled speculatively;
- preserve lexical shadowing, optimized location-list ownership, formal-parameter behavior, PIE/non-PIE execution, GCC/Clang-large compatibility, and clean exit.

## Later priorities

After Priority 4, aggregate/type presentation, richer expression evaluation, inlined-scope ownership, and DWARF5 location-list ownership remain candidates. They must continue to be ordered by concrete compiler-produced failures rather than surface-area breadth.

## Selection rule

Priority 4 is the current frontier. Select the smallest real pointer-local workflow whose location is already representable by the existing evaluator but whose compiler-produced type DIE is rejected by the integer-only type model. Prove that exact failure first under the permanent GCC and Clang-large gates, then add only the pointer type/value semantics needed by that workflow. Do not use pointer support as a reason to build aggregate rendering, arbitrary dereference expressions, or a general DWARF VM.
