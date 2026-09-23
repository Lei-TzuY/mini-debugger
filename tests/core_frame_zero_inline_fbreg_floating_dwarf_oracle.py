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


def require_f64(record, by_offset, context):
    base = unwrap_type(record, by_offset, context)
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context} is not a compiler base type")
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or numeric_attr(size, f"{context} byte size") != 8:
        raise RuntimeError(f"{context} is not exactly eight bytes")
    if encoding is None:
        raise RuntimeError(f"{context} has no compiler floating encoding")
    text = str(encoding).lower()
    if "float" not in text and numeric_attr(encoding, f"{context} encoding") != 4:
        raise RuntimeError(f"{context} is not DW_ATE_float")


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_fbreg_floating_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_fbreg_floating":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_fbreg_floating requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError(
            "inline_fbreg_floating has no compiler-produced DW_AT_location"
        )
    expression, ownership = exact_fbreg_location(
        path, location, probe, "inline_fbreg_floating"
    )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_fbreg_floating has no resolved DW_AT_type")
    value_type = by_offset.get(ref_offset(type_text, "inline_fbreg_floating type"))
    if value_type is None:
        raise RuntimeError(
            "inline_fbreg_floating type references an unknown DIE"
        )
    require_f64(value_type, by_offset, "inline_fbreg_floating type")

    print(
        "frame-zero inline fbreg floating DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} type=8-byte-f64"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_fbreg_floating_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(
        path, "snapshot_frame_zero_inline_fbreg_floating_probe"
    )
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_fbreg_floating_inner",
        "frame_zero_inline_fbreg_floating_outer",
        "frame_zero_inline_fbreg_floating_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(
            f"unexpected frame-zero fbreg floating chain: {chain}"
        )
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline fbreg floating DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
