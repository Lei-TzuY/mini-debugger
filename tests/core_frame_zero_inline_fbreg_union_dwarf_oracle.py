#!/usr/bin/env python3
import re
import subprocess
import sys

from core_inline_dwarf_oracle import (
    active_debug_loc_expression,
    addr2line_contexts,
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
            f"{context} active location is not one exact DW_OP_fbreg: {expression}"
        )
    return expression, f"loclist=[0x{begin:x},0x{end:x})"


def require_scalar_member(member, by_offset, expected_name, expected_encoding):
    if resolved_name(member, by_offset) != expected_name:
        raise RuntimeError(
            f"inline_fbreg_union member mismatch: expected {expected_name}"
        )
    location = member["attrs"].get("data_member_location")
    if location is not None and numeric_attr(
        location, f"inline_fbreg_union {expected_name} offset"
    ) != 0:
        raise RuntimeError(
            f"inline_fbreg_union member {expected_name} does not overlap at offset zero"
        )
    type_text = resolved_attr(member, by_offset, "type")
    if not type_text:
        raise RuntimeError(
            f"inline_fbreg_union {expected_name} has no resolved DW_AT_type"
        )
    value_type = by_offset.get(
        ref_offset(type_text, f"inline_fbreg_union {expected_name}")
    )
    if value_type is None:
        raise RuntimeError(f"inline_fbreg_union {expected_name} type is unknown")
    value_type = unwrap_type(
        value_type, by_offset, f"inline_fbreg_union {expected_name}"
    )
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(
            f"inline_fbreg_union {expected_name} is not a base type"
        )
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(
        size, f"inline_fbreg_union {expected_name} size"
    ) != 4:
        raise RuntimeError(
            f"inline_fbreg_union {expected_name} is not four bytes"
        )
    if not encoding or numeric_attr(
        encoding, f"inline_fbreg_union {expected_name} encoding"
    ) != expected_encoding:
        raise RuntimeError(
            f"inline_fbreg_union {expected_name} encoding changed"
        )


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_fbreg_union_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_fbreg_union":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_fbreg_union requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError(
            "inline_fbreg_union has no compiler-produced DW_AT_location"
        )
    expression, ownership = exact_fbreg_location(
        path, location, probe, "inline_fbreg_union"
    )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_fbreg_union has no resolved DW_AT_type")
    union_type = by_offset.get(ref_offset(type_text, "inline_fbreg_union type"))
    if union_type is None:
        raise RuntimeError("inline_fbreg_union type references an unknown DIE")
    union_type = unwrap_type(
        union_type, by_offset, "inline_fbreg_union type"
    )
    if union_type["tag"] != "DW_TAG_union_type":
        raise RuntimeError(
            "inline_fbreg_union does not resolve to DW_TAG_union_type"
        )
    size = union_type["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_fbreg_union byte size") != 4:
        raise RuntimeError(
            "inline_fbreg_union compiler union type is not four bytes"
        )

    members = [
        child
        for child in direct_children(records, union_type)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"inline_fbreg_union requires exactly two direct members, found {len(members)}"
        )
    require_scalar_member(members[0], by_offset, "signed_value", 5)
    require_scalar_member(members[1], by_offset, "unsigned_value", 7)

    print(
        "frame-zero inline fbreg union DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} size=4 "
        "members=signed_value:int32,unsigned_value:uint32"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_fbreg_union_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(
        path, "snapshot_frame_zero_inline_fbreg_union_probe"
    )
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_fbreg_union_inner",
        "frame_zero_inline_fbreg_union_outer",
        "frame_zero_inline_fbreg_union_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(
            f"unexpected frame-zero fbreg union chain: {chain}"
        )
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline fbreg union DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
