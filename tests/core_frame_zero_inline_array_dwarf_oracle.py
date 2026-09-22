#!/usr/bin/env python3
import subprocess
import sys

from core_inline_dwarf_oracle import (
    active_debug_loc_expression,
    addr2line_contexts,
    clean_name,
    numeric_attr,
    origin_name,
    parse_dies,
    ref_offset,
    resolved_attr,
    resolved_name,
    run,
    symbol_address,
    unwrap_type,
)


def direct_children(records, parent):
    result = []
    found = False
    for record in records:
        if record["offset"] == parent["offset"]:
            found = True
            continue
        if not found:
            continue
        if record["depth"] <= parent["depth"]:
            break
        if record["depth"] == parent["depth"] + 1:
            result.append(record)
    return result


def require_signed_i32(type_record, by_offset, context):
    value_type = unwrap_type(type_record, by_offset, context)
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: element is not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding", "")
    if not size or numeric_attr(size, f"{context} byte size") != 4:
        raise RuntimeError(f"{context}: element is not exactly four bytes")
    if "signed" not in encoding and numeric_attr(encoding, f"{context} encoding") != 5:
        raise RuntimeError(f"{context}: element is not a signed integer type")


def require_array_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_array_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_array":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            f"inline_array requires exactly one concrete active binding, found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_array has no compiler-produced DW_AT_location")
    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, "inline_array"
    )

    proven_shapes = {
        "DW_OP_reg1 (rdx); DW_OP_piece: 4; DW_OP_reg2 (rcx); DW_OP_piece: 4",
        "DW_OP_reg2 (rcx); DW_OP_piece: 4; DW_OP_reg1 (rdx); DW_OP_piece: 4",
    }
    if expression not in proven_shapes:
        raise RuntimeError(
            "inline_array active compiler location is not one of the two expected "
            "RDX/RCX four-byte piece orderings: " + expression
        )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_array has no resolved DW_AT_type")
    array_type = by_offset.get(ref_offset(type_text, "inline_array type"))
    if array_type is None:
        raise RuntimeError("inline_array type references an unknown DIE")
    array_type = unwrap_type(array_type, by_offset, "inline_array type")
    if array_type["tag"] != "DW_TAG_array_type":
        raise RuntimeError("inline_array does not resolve to DW_TAG_array_type")

    element_text = array_type["attrs"].get("type")
    if not element_text:
        raise RuntimeError("inline_array has no compiler element type")
    element = by_offset.get(ref_offset(element_text, "inline_array element"))
    if element is None:
        raise RuntimeError("inline_array element type references an unknown DIE")
    require_signed_i32(element, by_offset, "inline_array element")

    subranges = [
        child
        for child in direct_children(records, array_type)
        if child["tag"] == "DW_TAG_subrange_type"
    ]
    if len(subranges) != 1:
        raise RuntimeError(
            f"inline_array requires exactly one direct subrange, found {len(subranges)}"
        )
    subrange = subranges[0]
    lower = subrange["attrs"].get("lower_bound")
    if lower is not None and numeric_attr(lower, "inline_array lower bound") != 0:
        raise RuntimeError("inline_array lower bound is not zero")
    count = subrange["attrs"].get("count")
    upper = subrange["attrs"].get("upper_bound")
    if count is not None:
        element_count = numeric_attr(count, "inline_array count")
    elif upper is not None:
        element_count = numeric_attr(upper, "inline_array upper bound") + 1
    else:
        raise RuntimeError("inline_array has no bounded element count")
    if element_count != 2:
        raise RuntimeError(f"inline_array element count changed: {element_count}")

    declared_size = array_type["attrs"].get("byte_size")
    if declared_size is not None and numeric_attr(
        declared_size, "inline_array byte size"
    ) != 8:
        raise RuntimeError("inline_array declared byte size is not eight")

    print(
        "frame-zero inline array DWARF oracle passed: "
        f"die=0x{variable['offset']:x} range=[0x{begin:x},0x{end:x}) "
        f"location={expression} count=2 element=i32 size=8"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_array_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_array_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_array_inner",
        "frame_zero_inline_array_outer",
        "frame_zero_inline_array_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected frame-zero inline array chain: {chain}")
    require_array_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"frame-zero inline array DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
