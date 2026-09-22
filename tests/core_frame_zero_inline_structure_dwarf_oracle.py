#!/usr/bin/env python3
import re
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


def require_signed_i32(record, by_offset, context):
    type_text = resolved_attr(record, by_offset, "type")
    if not type_text:
        raise RuntimeError(f"{context}: no resolved DW_AT_type")
    value_type = by_offset.get(ref_offset(type_text, context))
    if value_type is None:
        raise RuntimeError(f"{context}: unknown type DIE")
    value_type = unwrap_type(value_type, by_offset, context)
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding", "")
    if not size or numeric_attr(size, f"{context} byte size") != 4:
        raise RuntimeError(f"{context}: not exactly four bytes")
    if "signed" not in encoding and numeric_attr(encoding, f"{context} encoding") != 5:
        raise RuntimeError(f"{context}: not a signed integer type")


def member_offset(member, context):
    value = member["attrs"].get("data_member_location")
    if value is None:
        raise RuntimeError(f"{context}: missing DW_AT_data_member_location")
    return numeric_attr(value, f"{context} offset")


def require_structure_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_structure_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_pair":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            f"inline_pair requires exactly one concrete active binding, found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_pair has no compiler-produced DW_AT_location")
    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, "inline_pair"
    )

    proven_shapes = {
        "DW_OP_reg1 (rdx); DW_OP_piece: 4; DW_OP_reg2 (rcx); DW_OP_piece: 4",
        "DW_OP_reg2 (rcx); DW_OP_piece: 4; DW_OP_reg1 (rdx); DW_OP_piece: 4",
    }
    if expression not in proven_shapes:
        raise RuntimeError(
            "inline_pair active compiler location is not one of the two proven "
            "RDX/RCX four-byte piece orderings: " + expression
        )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_pair has no resolved DW_AT_type")
    structure = by_offset.get(ref_offset(type_text, "inline_pair type"))
    if structure is None:
        raise RuntimeError("inline_pair type references an unknown DIE")
    structure = unwrap_type(structure, by_offset, "inline_pair type")
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("inline_pair does not resolve to DW_TAG_structure_type")
    size = structure["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_pair byte size") != 8:
        raise RuntimeError("inline_pair compiler structure is not exactly eight bytes")

    members = {
        clean_name(child["attrs"].get("name", "")): child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    }
    if set(members) != {"first", "second"}:
        raise RuntimeError(f"inline_pair direct members changed: {sorted(members)}")
    if member_offset(members["first"], "inline_pair.first") != 0:
        raise RuntimeError("inline_pair.first compiler offset is not zero")
    if member_offset(members["second"], "inline_pair.second") != 4:
        raise RuntimeError("inline_pair.second compiler offset is not four")
    require_signed_i32(members["first"], by_offset, "inline_pair.first")
    require_signed_i32(members["second"], by_offset, "inline_pair.second")

    print(
        "frame-zero inline structure DWARF oracle passed: "
        f"die=0x{variable['offset']:x} range=[0x{begin:x},0x{end:x}) "
        f"location={expression} size=8 first@0:i32 second@4:i32"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_structure_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_structure_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_structure_inner",
        "frame_zero_inline_structure_outer",
        "frame_zero_inline_structure_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected frame-zero inline structure chain: {chain}")
    require_structure_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"frame-zero inline structure DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
