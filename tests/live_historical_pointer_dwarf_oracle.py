#!/usr/bin/env python3
import re
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
    parse_records,
    run,
    scalar_number,
    type_reference,
    unwrap_type,
)


def find_formal_parameter(records, function_name, parameter_name):
    functions = [
        (index, record)
        for index, record in enumerate(records)
        if record["tag"] == "DW_TAG_subprogram"
        and clean_name(record["attrs"].get("name", "")) == function_name
    ]
    if len(functions) != 1:
        raise RuntimeError(
            f"{function_name}: expected exactly one subprogram DIE, found {len(functions)}"
        )
    function_index, function = functions[0]
    for record in records[function_index + 1 :]:
        if record["depth"] <= function["depth"]:
            break
        if record["tag"] != "DW_TAG_formal_parameter":
            continue
        if clean_name(record["attrs"].get("name", "")) == parameter_name:
            return record
    raise RuntimeError(
        f"{function_name}: {parameter_name} formal parameter DIE not found"
    )


def call_return_pc(path):
    text = run("objdump", "-d", "--disassemble=historical_pointer_caller", path)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if "call" not in line or "<historical_pointer_callee>" not in line:
            continue
        for following in lines[index + 1 :]:
            match = re.match(r"^\s*([0-9a-fA-F]+):", following)
            if match:
                return int(match.group(1), 16)
    raise RuntimeError(
        "historical_pointer_caller has no decodable call to historical_pointer_callee"
    )


def active_location(path, location, pc):
    if "(DW_OP_" in location and "location list" not in location:
        return location.split("(", 1)[1].rsplit(")", 1)[0], None

    match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not match:
        raise RuntimeError(
            "historical_pointer has unsupported compiler location representation: "
            + location
        )
    wanted = int(match.group(1), 16)
    loc = run("readelf", "--debug-dump=loc", path)
    active = False
    for line in loc.splitlines():
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
        begin = int(values[-2], 16)
        end = int(values[-1], 16)
        expression = expression.rsplit(")", 1)[0]
        if begin <= pc < end:
            return expression, (begin, end)
    raise RuntimeError(
        f"historical_pointer location list does not own caller return PC 0x{pc:x}"
    )


def verify(path):
    info = run("readelf", "--debug-dump=info", path)
    records = parse_records(info)
    variable = find_formal_parameter(
        records, "historical_pointer_caller", "historical_pointer"
    )
    if variable["tag"] != "DW_TAG_formal_parameter":
        raise RuntimeError("historical_pointer is not retained as a formal parameter")
    type_attr = variable["attrs"].get("type")
    location = variable["attrs"].get("location")
    if not type_attr or not location:
        raise RuntimeError(
            "historical_pointer is missing compiler type/location evidence"
        )

    pointer = unwrap_type(
        records,
        type_reference(type_attr, "historical_pointer"),
        "historical_pointer",
    )
    if pointer["tag"] != "DW_TAG_pointer_type":
        raise RuntimeError("historical_pointer is not a compiler pointer")
    pointer_size = pointer["attrs"].get("byte_size")
    if pointer_size and scalar_number(
        pointer_size, "historical pointer byte size"
    ) != 8:
        raise RuntimeError("historical pointer is not exactly eight bytes")

    pointee_attr = pointer["attrs"].get("type")
    if not pointee_attr:
        raise RuntimeError("historical pointer has no pointee type")
    pointee = unwrap_type(
        records,
        type_reference(pointee_attr, "historical pointer pointee"),
        "historical pointer pointee",
    )
    if pointee["tag"] != "DW_TAG_base_type":
        raise RuntimeError("historical pointer pointee is not a base type")
    size = pointee["attrs"].get("byte_size")
    encoding = pointee["attrs"].get("encoding")
    if not size or scalar_number(size, "historical pointer pointee size") != 4:
        raise RuntimeError("historical pointer pointee is not exactly four bytes")
    if encoding is None or scalar_number(
        encoding, "historical pointer pointee encoding"
    ) != 5:
        raise RuntimeError("historical pointer pointee is not signed int32")

    return_pc = call_return_pc(path)
    expression, owned_range = active_location(path, location, return_pc)
    match = re.fullmatch(r"DW_OP_reg(\d+) \(([^)]+)\)", expression)
    if not match:
        raise RuntimeError(
            "historical pointer requires one exact compiler register operation: "
            + expression
        )
    regno = int(match.group(1))
    regname = match.group(2)
    range_text = (
        "direct"
        if owned_range is None
        else f"[0x{owned_range[0]:x},0x{owned_range[1]:x})"
    )
    raise RuntimeError(
        "historical pointer register evidence: "
        f"return-pc=0x{return_pc:x} location={expression} "
        f"regno={regno} regname={regname} range={range_text}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: live_historical_pointer_dwarf_oracle.py <fixture>"
        )
    verify(sys.argv[1])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"historical live pointer DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
