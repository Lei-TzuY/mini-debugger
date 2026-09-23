#!/usr/bin/env python3
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    direct_children,
    find_variable,
    parse_records,
    run,
    scalar_number,
    symbol_addresses,
    type_reference,
    unwrap_type,
)
from core_xmm_dwarf_oracle import parse_dies as parse_basic_dies, verify_stack_local


def verify_layout(records):
    variable = find_variable(records, "inspect_live_array", "live_array")
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_array has no DW_AT_type")
    array = unwrap_type(
        records, type_reference(type_attr, "live_array"), "live_array"
    )
    if array["tag"] != "DW_TAG_array_type":
        raise RuntimeError("live_array is not a compiler array type")

    element_attr = array["attrs"].get("type")
    if not element_attr:
        raise RuntimeError("live_array has no compiler element type")
    element = unwrap_type(
        records,
        type_reference(element_attr, "live_array element"),
        "live_array element",
    )
    if element["tag"] != "DW_TAG_base_type":
        raise RuntimeError("live_array element is not a compiler base type")
    size = element["attrs"].get("byte_size")
    encoding = element["attrs"].get("encoding")
    if not size or scalar_number(size, "live_array element size") != 4:
        raise RuntimeError("live_array element is not exactly four bytes")
    if encoding is None or scalar_number(encoding, "live_array element encoding") != 5:
        raise RuntimeError("live_array element is not compiler-described signed int32")

    subranges = [
        child for child in direct_children(records, array)
        if child["tag"] == "DW_TAG_subrange_type"
    ]
    if len(subranges) != 1:
        raise RuntimeError(
            f"live_array requires exactly one direct subrange, found {len(subranges)}"
        )
    subrange = subranges[0]
    lower = subrange["attrs"].get("lower_bound")
    if lower is not None and scalar_number(lower, "live_array lower bound") != 0:
        raise RuntimeError("live_array lower bound is not zero")

    count = subrange["attrs"].get("count")
    upper = subrange["attrs"].get("upper_bound")
    if count is not None:
        element_count = scalar_number(count, "live_array element count")
        if upper is not None and scalar_number(
            upper, "live_array upper bound"
        ) + 1 != element_count:
            raise RuntimeError("live_array count conflicts with upper bound")
    elif upper is not None:
        element_count = scalar_number(upper, "live_array upper bound") + 1
    else:
        raise RuntimeError("live_array has no bounded compiler element count")
    if element_count != 3:
        raise RuntimeError(
            f"live_array compiler element count changed: {element_count}"
        )

    declared = array["attrs"].get("byte_size")
    if declared is not None and scalar_number(
        declared, "live_array byte size"
    ) != 12:
        raise RuntimeError("live_array declared size is not twelve bytes")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_array_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "inspect_live_array",
        "live_array",
        "live_array_probe",
        loc,
        basic_records,
        symbols,
    )
    verify_layout(records)
    print(
        "live fixed-array DWARF oracle passed: "
        "type=int32_t[3] bytes=12 location=DW_OP_fbreg"
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"live fixed-array DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
