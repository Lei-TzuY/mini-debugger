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


def enum_representation(enum_type, by_offset):
    size = enum_type["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_fbreg_mode byte size") != 4:
        raise RuntimeError("inline_fbreg_mode enum is not exactly four bytes")

    encoding = enum_type["attrs"].get("encoding")
    if encoding is not None:
        code = numeric_attr(encoding, "inline_fbreg_mode direct encoding")
        if code != 7:
            raise RuntimeError(
                "inline_fbreg_mode direct compiler representation is not unsigned"
            )
        return "direct-encoding"

    underlying_text = enum_type["attrs"].get("type")
    if not underlying_text:
        raise RuntimeError(
            "inline_fbreg_mode has neither direct encoding nor underlying type"
        )
    underlying = by_offset.get(
        ref_offset(underlying_text, "inline_fbreg_mode underlying type")
    )
    if underlying is None:
        raise RuntimeError("inline_fbreg_mode underlying type is unknown")
    underlying = unwrap_type(
        underlying, by_offset, "inline_fbreg_mode underlying type"
    )
    if underlying["tag"] != "DW_TAG_base_type":
        raise RuntimeError("inline_fbreg_mode underlying type is not a base type")
    underlying_size = underlying["attrs"].get("byte_size")
    underlying_encoding = underlying["attrs"].get("encoding")
    if not underlying_size or numeric_attr(
        underlying_size, "inline_fbreg_mode underlying size"
    ) != 4:
        raise RuntimeError("inline_fbreg_mode underlying type is not four bytes")
    if not underlying_encoding or numeric_attr(
        underlying_encoding, "inline_fbreg_mode underlying encoding"
    ) != 7:
        raise RuntimeError("inline_fbreg_mode underlying type is not unsigned")
    return "underlying-type"


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_fbreg_enum_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_fbreg_mode":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_fbreg_mode requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )

    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError(
            "inline_fbreg_mode has no compiler-produced DW_AT_location"
        )
    expression, ownership = exact_fbreg_location(
        path, location, probe, "inline_fbreg_mode"
    )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_fbreg_mode has no resolved DW_AT_type")
    enum_type = by_offset.get(ref_offset(type_text, "inline_fbreg_mode type"))
    if enum_type is None:
        raise RuntimeError("inline_fbreg_mode type references an unknown DIE")
    enum_type = unwrap_type(enum_type, by_offset, "inline_fbreg_mode type")
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError(
            "inline_fbreg_mode does not resolve to DW_TAG_enumeration_type"
        )

    name = enum_type["attrs"].get("name", "")
    if "FrameZeroInlineFbregMode" not in name:
        raise RuntimeError(
            f"inline_fbreg_mode enum name changed: {name!r}"
        )
    representation = enum_representation(enum_type, by_offset)

    expected = {
        "FrameZeroInlineFbregIdle": 3,
        "FrameZeroInlineFbregReady": 7,
        "FrameZeroInlineFbregBusy": 42,
    }
    actual = {}
    for child in direct_children(records, enum_type):
        if child["tag"] != "DW_TAG_enumerator":
            continue
        entry_name = child["attrs"].get("name", "")
        entry_name = re.sub(
            r"^\(indirect string, offset: 0x[0-9a-fA-F]+\):\s*",
            "",
            entry_name,
        )
        value = child["attrs"].get("const_value")
        if entry_name and value is not None:
            actual[entry_name] = numeric_attr(
                value, f"{entry_name} const_value"
            )
    if actual != expected:
        raise RuntimeError(
            f"inline_fbreg_mode compiler enumerator table mismatch: {actual}"
        )

    print(
        "frame-zero inline fbreg enum DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} size=4 unsigned "
        f"representation={representation} busy=42"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_fbreg_enum_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(
        path, "snapshot_frame_zero_inline_fbreg_enum_probe"
    )
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_fbreg_enum_inner",
        "frame_zero_inline_fbreg_enum_outer",
        "frame_zero_inline_fbreg_enum_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(
            f"unexpected frame-zero fbreg enum chain: {chain}"
        )
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline fbreg enum DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
