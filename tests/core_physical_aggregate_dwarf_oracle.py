#!/usr/bin/env python3
import re
import subprocess
import sys

from core_xmm_dwarf_oracle import (
    parse_dies as parse_basic_dies,
    run,
    symbol_addresses,
    verify_stack_local,
)


def clean_name(value):
    marker = "): "
    if marker in value:
        return value.rsplit(marker, 1)[1].strip()
    return value.strip()


def parse_records(text):
    records = []
    current = None
    header_re = re.compile(
        r"^\s*<(\d+)><([0-9a-fA-F]+)>:\s+Abbrev Number:\s+\d+\s+\((DW_TAG_[^)]+)\)"
    )
    attr_re = re.compile(r"^\s*<[0-9a-fA-F]+>\s+DW_AT_([^\s]+)\s*:\s*(.*)$")
    for line in text.splitlines():
        header = header_re.match(line)
        if header:
            if current is not None:
                records.append(current)
            current = {
                "depth": int(header.group(1)),
                "offset": int(header.group(2), 16),
                "tag": header.group(3),
                "attrs": {},
            }
            continue
        if current is None:
            continue
        attr = attr_re.match(line)
        if attr:
            current["attrs"][attr.group(1)] = attr.group(2).strip()
    if current is not None:
        records.append(current)
    return records


def scalar_number(value, context):
    hex_matches = re.findall(r"0x([0-9a-fA-F]+)", value)
    if hex_matches:
        return int(hex_matches[-1], 16)
    decimal_matches = re.findall(r"(?<![A-Za-z0-9_])(\d+)(?![A-Za-z0-9_])", value)
    if decimal_matches:
        return int(decimal_matches[-1], 10)
    raise RuntimeError(f"{context}: numeric attribute is unavailable: {value}")


def type_reference(value, context):
    match = re.search(r"<(?:0x)?([0-9a-fA-F]+)>", value)
    if not match:
        raise RuntimeError(f"{context}: type reference is unavailable: {value}")
    return int(match.group(1), 16)


def record_by_offset(records, offset, context):
    for record in records:
        if record["offset"] == offset:
            return record
    raise RuntimeError(f"{context}: referenced DIE 0x{offset:x} is unavailable")


def unwrap_type(records, offset, context):
    for _ in range(16):
        record = record_by_offset(records, offset, context)
        if record["tag"] not in {
            "DW_TAG_typedef",
            "DW_TAG_const_type",
            "DW_TAG_volatile_type",
            "DW_TAG_restrict_type",
        }:
            return record
        type_attr = record["attrs"].get("type")
        if not type_attr:
            raise RuntimeError(f"{context}: wrapper has no DW_AT_type")
        offset = type_reference(type_attr, context)
    raise RuntimeError(f"{context}: type wrapper chain is too deep")


def find_subprogram(records, function_name):
    for index, record in enumerate(records):
        if record["tag"] == "DW_TAG_subprogram" and clean_name(
            record["attrs"].get("name", "")
        ) == function_name:
            return index, record
    raise RuntimeError(f"{function_name}: subprogram DIE not found")


def find_variable(records, function_name, variable_name):
    index, function = find_subprogram(records, function_name)
    for child in records[index + 1 :]:
        if child["depth"] <= function["depth"]:
            break
        if child["tag"] != "DW_TAG_variable":
            continue
        if clean_name(child["attrs"].get("name", "")) == variable_name:
            return child
    raise RuntimeError(f"{function_name}: {variable_name} DIE not found")


def direct_children(records, owner):
    result = []
    seen = False
    for record in records:
        if record["offset"] == owner["offset"]:
            seen = True
            continue
        if not seen:
            continue
        if record["depth"] <= owner["depth"]:
            break
        if record["depth"] == owner["depth"] + 1:
            result.append(record)
    return result


def require_unsigned_u64(records, member, context):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{context}: member has no DW_AT_type")
    base = unwrap_type(records, type_reference(type_attr, context), context)
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: member does not resolve to a base type")
    size_attr = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding", "")
    if not size_attr or scalar_number(size_attr, context) != 8:
        raise RuntimeError(f"{context}: member is not exactly eight bytes")
    if "unsigned" not in encoding.lower():
        raise RuntimeError(f"{context}: member is not compiler-described unsigned")


def verify_layout(records):
    variable = find_variable(
        records, "caller_with_stack_local", "caller_stack_aggregate"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_stack_aggregate has no DW_AT_type")
    structure = unwrap_type(
        records,
        type_reference(type_attr, "caller_stack_aggregate"),
        "caller_stack_aggregate",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("caller_stack_aggregate is not a compiler structure type")
    size_attr = structure["attrs"].get("byte_size")
    if not size_attr or scalar_number(size_attr, "caller_stack_aggregate size") != 16:
        raise RuntimeError("caller_stack_aggregate is not exactly sixteen bytes")

    members = [
        child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError("caller_stack_aggregate does not have exactly two direct members")

    expected = {"first": 0, "second": 8}
    actual = {}
    for member in members:
        name = clean_name(member["attrs"].get("name", ""))
        if name not in expected:
            raise RuntimeError(f"unexpected caller aggregate member: {name}")
        location = member["attrs"].get("data_member_location")
        if location is None:
            raise RuntimeError(f"{name}: missing DW_AT_data_member_location")
        offset = scalar_number(location, f"{name} offset")
        require_unsigned_u64(records, member, name)
        actual[name] = offset
    if actual != expected:
        raise RuntimeError(f"caller aggregate layout mismatch: {actual}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_physical_aggregate_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_stack_aggregate",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_layout(records)
    print("physical stack aggregate DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical stack aggregate DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
