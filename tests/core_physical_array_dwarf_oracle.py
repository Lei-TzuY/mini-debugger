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
    variable = find_variable(records, "caller_with_stack_local", "caller_fixed_array")
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_fixed_array has no DW_AT_type")
    array = unwrap_type(
        records,
        type_reference(type_attr, "caller_fixed_array"),
        "caller_fixed_array",
    )
    if array["tag"] != "DW_TAG_array_type":
        raise RuntimeError("caller_fixed_array is not a compiler array type")

    element_attr = array["attrs"].get("type")
    if not element_attr:
        raise RuntimeError("caller_fixed_array array type has no element type")
    element = unwrap_type(
        records,
        type_reference(element_attr, "caller_fixed_array element"),
        "caller_fixed_array element",
    )
    if element["tag"] != "DW_TAG_base_type":
        raise RuntimeError("caller_fixed_array element is not a compiler base type")
    size_attr = element["attrs"].get("byte_size")
    encoding = element["attrs"].get("encoding")
    if not size_attr or scalar_number(size_attr, "array element byte size") != 4:
        raise RuntimeError("caller_fixed_array element is not exactly four bytes")
    if encoding is None or scalar_number(encoding, "array element encoding") != 5:
        raise RuntimeError("caller_fixed_array element is not compiler-described signed int32")

    subranges = [
        child
        for child in direct_children(records, array)
        if child["tag"] == "DW_TAG_subrange_type"
    ]
    if len(subranges) != 1:
        raise RuntimeError(
            f"caller_fixed_array requires exactly one direct subrange, found {len(subranges)}"
        )
    subrange = subranges[0]
    lower = subrange["attrs"].get("lower_bound")
    if lower is not None and scalar_number(lower, "array lower bound") != 0:
        raise RuntimeError("caller_fixed_array lower bound is not zero")

    count = subrange["attrs"].get("count")
    upper = subrange["attrs"].get("upper_bound")
    if count is not None:
        element_count = scalar_number(count, "array element count")
        if upper is not None and scalar_number(upper, "array upper bound") + 1 != element_count:
            raise RuntimeError("caller_fixed_array count conflicts with upper bound")
    elif upper is not None:
        element_count = scalar_number(upper, "array upper bound") + 1
    else:
        raise RuntimeError("caller_fixed_array has no bounded compiler element count")
    if element_count != 3:
        raise RuntimeError(
            f"caller_fixed_array compiler element count changed: {element_count}"
        )

    total_size = element_count * 4
    declared = array["attrs"].get("byte_size")
    if declared is not None and scalar_number(declared, "array byte size") != total_size:
        raise RuntimeError("caller_fixed_array declared size conflicts with element layout")
    if total_size != 12:
        raise RuntimeError("caller_fixed_array total byte extent is not exactly twelve bytes")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_physical_array_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_fixed_array",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_layout(records)
    print("physical fixed-array DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical fixed-array DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
