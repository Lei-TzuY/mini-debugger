#!/usr/bin/env python3
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
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


def require_signed_i32(records, member, context):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{context}: member has no DW_AT_type")
    base = unwrap_type(records, type_reference(type_attr, context), context)
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: member does not resolve to a base type")
    size_attr = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size_attr or scalar_number(size_attr, context) != 4:
        raise RuntimeError(f"{context}: member is not exactly four bytes")
    if encoding is None or scalar_number(encoding, context) != 5:
        raise RuntimeError(f"{context}: member is not compiler-described signed int32")


def member_map(records, structure, context):
    members = [
        child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    result = {}
    for member in members:
        name = clean_name(member["attrs"].get("name", ""))
        if not name or name in result:
            raise RuntimeError(f"{context}: invalid or duplicate direct member name")
        result[name] = member
    return result


def member_offset(member, context):
    value = member["attrs"].get("data_member_location")
    if value is None:
        raise RuntimeError(f"{context}: missing DW_AT_data_member_location")
    return scalar_number(value, context)


def verify_layout(records):
    variable = find_variable(
        records, "caller_with_stack_local", "caller_nested_aggregate"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_nested_aggregate has no DW_AT_type")
    outer = unwrap_type(
        records,
        type_reference(type_attr, "caller_nested_aggregate"),
        "caller_nested_aggregate",
    )
    if outer["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("caller_nested_aggregate is not a structure type")
    outer_size = outer["attrs"].get("byte_size")
    if not outer_size or scalar_number(outer_size, "outer size") != 8:
        raise RuntimeError("physical nested outer structure is not exactly eight bytes")

    outer_members = member_map(records, outer, "physical nested outer")
    if set(outer_members) != {"prefix", "inner"}:
        raise RuntimeError(
            f"physical nested outer members changed: {sorted(outer_members)}"
        )
    if member_offset(outer_members["prefix"], "prefix offset") != 0:
        raise RuntimeError("physical nested prefix is not at offset zero")
    if member_offset(outer_members["inner"], "inner offset") != 4:
        raise RuntimeError("physical nested inner is not at offset four")
    require_signed_i32(records, outer_members["prefix"], "physical nested prefix")

    inner_type = outer_members["inner"]["attrs"].get("type")
    if not inner_type:
        raise RuntimeError("physical nested inner has no type")
    inner = unwrap_type(
        records, type_reference(inner_type, "physical nested inner"), "physical nested inner"
    )
    if inner["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("physical nested inner is not a structure type")
    inner_size = inner["attrs"].get("byte_size")
    if not inner_size or scalar_number(inner_size, "inner size") != 4:
        raise RuntimeError("physical nested inner structure is not exactly four bytes")

    inner_members = member_map(records, inner, "physical nested inner")
    if set(inner_members) != {"terminal"}:
        raise RuntimeError(
            f"physical nested inner members changed: {sorted(inner_members)}"
        )
    if member_offset(inner_members["terminal"], "terminal offset") != 0:
        raise RuntimeError("physical nested terminal is not at offset zero")
    require_signed_i32(records, inner_members["terminal"], "physical nested terminal")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_physical_nested_aggregate_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_nested_aggregate",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_layout(records)
    print("physical nested aggregate DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical nested aggregate DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
