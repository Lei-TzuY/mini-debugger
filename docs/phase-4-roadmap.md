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

## Priority 4: bounded pointer scalar inspection — complete

The fifth source-value slice extends the existing typed-value path across one concrete compiler-produced pointer type without broadening the location VM:

- a dedicated `-O0 -g -gdwarf-4` fixture keeps a stack-resident `uint64_t *local_pointer` live at the exported `local_pointer_probe`, pointing at the real runtime object `pointer_target`;
- test-first CI was exact: GCC 13.3 and Clang 18.1 both configured and built successfully, 42/44 existing tests stayed green, and only `pointer_value_integration_{pie,nopie}` failed with `local variable type is not a supported typedef/base-type chain`, proving the breakpoint, module routing, lexical ownership, and existing location evaluator had already succeeded and the rejected boundary was `DW_TAG_pointer_type`;
- the value result grows an explicit `LocalValueKind` and a generic `inspect_local_value` entry point, while `inspect_local_integer` remains as an integer-only compatibility wrapper rather than silently accepting pointer values as integers;
- pointer type resolution follows the same bounded typedef chain, accepts only the compiler-proven `DW_TAG_pointer_type`, requires the x86-64 eight-byte pointer width, and requires a `DW_FORM_ref4` pointee that itself resolves through the already supported integer typedef/base-type chain; malformed links, other widths, aggregates, references, arrays, pointer-to-pointer expansion, and unsupported pointees still fail explicitly;
- no new location expression or location-list rule is added: the pointer uses the existing selected-process/TID `DW_OP_fbreg` path and final memory read, so source-value ownership remains shared with the integer implementation;
- direct API integration compares the recovered raw pointer value with the independently resolved runtime ELF address of `pointer_target`, preserves owning-module identity and pointer width/kind metadata, and continues to clean exit;
- real `mdbg` subprocess integration renders `local_pointer = 0x...` deterministically in hexadecimal; pointer dereference is deliberately not bundled into this scalar milestone;
- PIE and non-PIE pass under both permanent GCC and Clang-large gates while all earlier integer, formal-parameter, optimized-location, and lexical-shadowing coverage remains green.

Priority 4 therefore closes the bounded scalar type-model gap without turning `print` into an aggregate renderer or a general DWARF expression engine.

## Priority 5: bounded struct aggregate presentation — current frontier

The next architectural gap is aggregate type/value shaping rather than another scalar variant. The current source-value resolver explicitly rejects `DW_TAG_structure_type`, even though a normal O0 local struct can be owned by the already supported lexical and `DW_OP_fbreg` location path.

Current Priority 5 acceptance:

- add one deterministic compiler-produced local struct with at least two independently verifiable integer members that remain live at an exported probe, and first prove the current evaluator reaches the struct type boundary while all pre-existing source-value and debugger tests remain green under GCC and Clang-large;
- extend the existing typed source-value model rather than introducing an aggregate-specific ptrace path: preserve owning module, local name, lexical ownership, selected process/TID, and the same location evaluator used by Priorities 0–4;
- support only the concrete `DW_TAG_structure_type` / direct-child `DW_TAG_member` shape emitted by that fixture, including bounded structure byte size, member names, `DW_FORM_ref4` integer member types, constant `DW_AT_data_member_location` offsets, and strict member-in-structure bounds validation;
- read the aggregate storage through the existing debugger memory API and decode the proven direct members from that owned byte range; do not separately peek each field through raw ptrace or duplicate the location engine;
- expose deterministic CLI presentation for the named members and prove the decoded values match the live runtime struct; padding may be skipped by offsets, but unions, arrays, bitfields, anonymous members, inheritance, nested aggregates, value pieces, and arbitrary expression-based member locations remain outside this slice;
- preserve pointer/integer scalar behavior, lexical shadowing, optimized location-list ownership, formal parameters, PIE/non-PIE execution, GCC/Clang-large compatibility, module identity, and clean exit.

## Later priorities

After Priority 5, richer expression evaluation, inlined-scope ownership, DWARF5 location-list ownership, and broader aggregate/type presentation remain candidates. They must continue to be ordered by concrete compiler-produced failures rather than surface-area breadth.

## Selection rule

Priority 5 is the current frontier. Select the smallest real local-struct workflow whose variable location is already representable by the existing evaluator but whose compiler-produced `DW_TAG_structure_type` is rejected by the scalar type model. Prove that exact failure first under the permanent GCC and Clang-large gates, then add only the structure/member metadata and owned-byte decoding needed by that workflow. Do not use aggregate support as a reason to add unions, arrays, bitfields, nested aggregate recursion, arbitrary dereference, or a general DWARF VM.
