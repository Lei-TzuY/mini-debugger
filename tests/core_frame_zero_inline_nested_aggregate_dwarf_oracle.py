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


def exact_fbreg_location(path, location, probe, context):
    direct = re.fullmatch(
        r"(?:\d+\s+byte block:\s+(?:[0-9a-fA-F]{1,2}\s+)+)?"
        r"\(?\s*(DW_OP_fbreg:\s*-?\d+)\s*\)?",
        location,
    )
    if direct:
        return re.sub(r"\s+", " ", direct.group(1)).strip(), "direct-exprloc"

    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, context
    )
    if not re.fullmatch(r"DW_OP_fbreg: -?\d+", expression):
        raise RuntimeError(
            f"{context} active compiler location is not one exact "
            f"DW_OP_fbreg operation: {expression}"
        )
    return expression, f"loclist=[0x{begin:x},0x{end:x})"


def member_offset(member, context):
    location = member["attrs"].get("data_member_location")
    if location is None:
        raise RuntimeError(f"{context} has no DW_AT_data_member_location")
    return numeric_attr(location, f"{context} offset")


def require_signed_int32(member, by_offset, context):
    type_text = resolved_attr(member, by_offset, "type")
    if not type_text:
        raise RuntimeError(f"{context}: missing resolved DW_AT_type")
    value_type = by_offset.get(ref_offset(type_text, f"{context} type"))
    if value_type is None:
        raise RuntimeError(f"{context}: type references an unknown DIE")
    value_type = unwrap_type(value_type, by_offset, f"{context} type")
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, f"{context} byte size") != 4:
        raise RuntimeError(f"{context}: not exactly four bytes")
    if not encoding or numeric_attr(encoding, f"{context} encoding") != 5:
        raise RuntimeError(f"{context}: not a signed integer")


def structure_from_member(member, by_offset, context):
    type_text = resolved_attr(member, by_offset, "type")
    if not type_text:
        raise RuntimeError(f"{context}: missing resolved DW_AT_type")
    structure = by_offset.get(ref_offset(type_text, f"{context} type"))
    if structure is None:
        raise RuntimeError(f"{context}: type references an unknown DIE")
    structure = unwrap_type(structure, by_offset, f"{context} type")
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError(f"{context}: not a compiler structure type")
    return structure


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_nested_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_nested":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_nested requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_nested has no compiler-produced DW_AT_location")
    expression, ownership = exact_fbreg_location(
        path, location, probe, "inline_nested"
    )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_nested has no resolved DW_AT_type")
    outer = by_offset.get(ref_offset(type_text, "inline_nested type"))
    if outer is None:
        raise RuntimeError("inline_nested type references an unknown DIE")
    outer = unwrap_type(outer, by_offset, "inline_nested type")
    if outer["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("inline_nested does not resolve to a structure")
    if clean_name(outer["attrs"].get("name", "")) != "FrameZeroInlineNestedOuter":
        raise RuntimeError("inline_nested outer type name changed")
    outer_size = outer["attrs"].get("byte_size")
    if not outer_size or numeric_attr(outer_size, "inline_nested outer byte size") != 8:
        raise RuntimeError("inline_nested outer structure is not exactly eight bytes")

    outer_members = {
        clean_name(child["attrs"].get("name", "")): child
        for child in direct_children(records, outer)
        if child["tag"] == "DW_TAG_member"
    }
    if set(outer_members) != {"prefix", "inner"}:
        raise RuntimeError(
            f"inline_nested outer members changed: {sorted(outer_members)}"
        )
    if member_offset(outer_members["prefix"], "inline_nested prefix") != 0:
        raise RuntimeError("inline_nested prefix is not at offset zero")
    if member_offset(outer_members["inner"], "inline_nested inner") != 4:
        raise RuntimeError("inline_nested inner is not at offset four")
    require_signed_int32(outer_members["prefix"], by_offset, "inline_nested prefix")

    inner = structure_from_member(
        outer_members["inner"], by_offset, "inline_nested inner"
    )
    if clean_name(inner["attrs"].get("name", "")) != "FrameZeroInlineNestedInner":
        raise RuntimeError("inline_nested inner type name changed")
    inner_size = inner["attrs"].get("byte_size")
    if not inner_size or numeric_attr(inner_size, "inline_nested inner byte size") != 4:
        raise RuntimeError("inline_nested inner structure is not exactly four bytes")
    inner_members = [
        child
        for child in direct_children(records, inner)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(inner_members) != 1 or clean_name(
        inner_members[0]["attrs"].get("name", "")
    ) != "terminal":
        raise RuntimeError("inline_nested inner terminal member changed")
    if member_offset(inner_members[0], "inline_nested terminal") != 0:
        raise RuntimeError("inline_nested terminal is not at offset zero")
    require_signed_int32(inner_members[0], by_offset, "inline_nested terminal")

    print(
        "frame-zero inline nested aggregate DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} "
        "outer=FrameZeroInlineNestedOuter:8 prefix@0:signed-int32 "
        "inner@4=FrameZeroInlineNestedInner:4 terminal@0:signed-int32"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_nested_aggregate_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_nested_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_nested_inner",
        "frame_zero_inline_nested_outer",
        "frame_zero_inline_nested_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(
            f"unexpected frame-zero inline nested chain: {chain}"
        )
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline nested aggregate DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
