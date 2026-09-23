#!/usr/bin/env python3
import re
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
    direct_children,
    parse_records,
    run,
    scalar_number,
    type_reference,
    unwrap_type,
)


def find_parameter(records):
    funcs = [
        (i, r) for i, r in enumerate(records)
        if r["tag"] == "DW_TAG_subprogram"
        and clean_name(r["attrs"].get("name", "")) == "historical_structure_caller"
    ]
    if len(funcs) != 1:
        raise RuntimeError(f"expected one historical_structure_caller DIE, found {len(funcs)}")
    index, function = funcs[0]
    for record in records[index + 1:]:
        if record["depth"] <= function["depth"]:
            break
        if record["tag"] == "DW_TAG_formal_parameter" and            clean_name(record["attrs"].get("name", "")) == "historical_structure_pointer":
            return record
    raise RuntimeError("historical_structure_pointer formal parameter DIE not found")


def call_return_pc(path):
    text = run("objdump", "-d", "--disassemble=historical_structure_caller", path)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if "call" not in line or "<historical_structure_callee>" not in line:
            continue
        for following in lines[index + 1:]:
            match = re.match(r"^\s*([0-9a-fA-F]+):", following)
            if match:
                return int(match.group(1), 16)
    raise RuntimeError("historical_structure_caller has no decodable callee call")


def active_location(path, location, pc):
    if "(DW_OP_" in location and "location list" not in location:
        return location.split("(", 1)[1].rsplit(")", 1)[0], None
    match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not match:
        raise RuntimeError("unsupported historical structure pointer location: " + location)
    wanted = int(match.group(1), 16)
    text = run("readelf", "--debug-dump=loc", path)
    active = False
    for line in text.splitlines():
        offset = re.match(r"^\s*([0-9a-fA-F]{8,16})\b", line)
        if not active and offset and int(offset.group(1), 16) == wanted:
            active = True
        if not active:
            continue
        if "<End of list>" in line:
            break
        if "(DW_OP_" not in line:
            continue
        prefix, expression = line.split("(", 1)
        values = re.findall(r"\b[0-9a-fA-F]{8,16}\b", prefix)
        if len(values) < 2:
            continue
        begin, end = int(values[-2], 16), int(values[-1], 16)
        expression = expression.rsplit(")", 1)[0]
        if begin <= pc < end:
            return expression, (begin, end)
    raise RuntimeError(f"structure pointer location list does not own return PC 0x{pc:x}")


def require_integer(records, member, name, offset, size, encoding):
    if clean_name(member["attrs"].get("name", "")) != name:
        raise RuntimeError(f"expected member {name}")
    location = member["attrs"].get("data_member_location")
    if location is None or scalar_number(location, f"{name} offset") != offset:
        raise RuntimeError(f"{name}: compiler offset changed")
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{name}: missing type")
    base = unwrap_type(records, type_reference(type_attr, name), name)
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{name}: not a base type")
    byte_size = base["attrs"].get("byte_size")
    enc = base["attrs"].get("encoding")
    if not byte_size or scalar_number(byte_size, f"{name} size") != size:
        raise RuntimeError(f"{name}: wrong byte size")
    if enc is None or scalar_number(enc, f"{name} encoding") != encoding:
        raise RuntimeError(f"{name}: wrong integer encoding")


def verify(path):
    records = parse_records(run("readelf", "--debug-dump=info", path))
    parameter = find_parameter(records)
    type_attr = parameter["attrs"].get("type")
    location = parameter["attrs"].get("location")
    if not type_attr or not location:
        raise RuntimeError("historical structure pointer lacks compiler type/location evidence")

    pointer = unwrap_type(
        records,
        type_reference(type_attr, "historical structure pointer"),
        "historical structure pointer",
    )
    if pointer["tag"] != "DW_TAG_pointer_type":
        raise RuntimeError("historical structure root is not a compiler pointer")
    pointer_size = pointer["attrs"].get("byte_size")
    if pointer_size and scalar_number(pointer_size, "pointer byte size") != 8:
        raise RuntimeError("historical structure pointer is not eight bytes")

    pointee_attr = pointer["attrs"].get("type")
    if not pointee_attr:
        raise RuntimeError("historical structure pointer has no pointee")
    structure = unwrap_type(
        records,
        type_reference(pointee_attr, "historical structure pointee"),
        "historical structure pointee",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("historical structure pointer pointee is not a structure")
    size = structure["attrs"].get("byte_size")
    if not size or scalar_number(size, "HistoricalLivePair byte size") != 16:
        raise RuntimeError("HistoricalLivePair is not exactly sixteen bytes")
    members = [c for c in direct_children(records, structure) if c["tag"] == "DW_TAG_member"]
    if len(members) != 2:
        raise RuntimeError(f"HistoricalLivePair requires two direct members, found {len(members)}")
    require_integer(records, members[0], "count", 0, 4, 7)
    require_integer(records, members[1], "delta", 8, 8, 5)

    pc = call_return_pc(path)
    expression, owned = active_location(path, location, pc)
    match = re.fullmatch(r"DW_OP_reg(\d+) \(([^)]+)\)", expression)
    if not match:
        raise RuntimeError("historical structure pointer is not one exact register op: " + expression)
    range_text = "direct" if owned is None else f"[0x{owned[0]:x},0x{owned[1]:x})"
    print(
        "historical live structure pointer DWARF oracle passed: "
        f"return-pc=0x{pc:x} location={expression} range={range_text} "
        "pointee=HistoricalLivePair{count@0:u32,delta@8:i64}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_historical_structure_pointer_dwarf_oracle.py <fixture>")
    verify(sys.argv[1])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"historical live structure pointer DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
