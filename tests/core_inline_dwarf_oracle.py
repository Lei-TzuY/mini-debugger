#!/usr/bin/env python3
import os
import re
import signal
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def clean_name(value):
    marker = "): "
    if marker in value:
        return value.rsplit(marker, 1)[1].strip()
    return value.strip()


def parse_dies(text):
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


def numeric_attr(value, context):
    matches = re.findall(r"0x([0-9a-fA-F]+)", value)
    if not matches:
        matches = re.findall(r"\b(\d+)\b", value)
    if not matches:
        raise RuntimeError(f"{context}: numeric attribute is unavailable: {value}")
    token = matches[-1]
    return int(token, 16 if "0x" in value else 10)


def ref_offset(value, context):
    match = re.search(r"<0x([0-9a-fA-F]+)>", value)
    if not match:
        raise RuntimeError(f"{context}: DIE reference is unavailable: {value}")
    return int(match.group(1), 16)


def symbol_address(path, name):
    for line in run("nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[2] == name:
            return int(fields[0], 16)
    raise RuntimeError(f"missing probe symbol: {name}")


def origin_name(record, by_offset):
    origin_text = record["attrs"].get("abstract_origin")
    if not origin_text:
        return None
    origin = by_offset.get(ref_offset(origin_text, "inline abstract_origin"))
    if origin is None or origin["tag"] != "DW_TAG_subprogram":
        raise RuntimeError("inline abstract origin does not reference a subprogram")
    name = clean_name(origin["attrs"].get("name", ""))
    return name or None


def addr2line_contexts(path, probe):
    lines = run("addr2line", "-i", "-f", "-e", path, hex(probe)).splitlines()
    if len(lines) < 2 or len(lines) % 2 != 0:
        raise RuntimeError("addr2line emitted an incomplete inline chain")
    return [
        (lines[index].strip(), lines[index + 1].strip())
        for index in range(0, len(lines), 2)
    ]


def location_basename(location):
    source = location.rsplit(":", 1)[0]
    return os.path.basename(source)


def require_mdbg_core_callsite_ownership(path):
    mdbg_core = os.environ.get("MDBG_CORE", "build/mdbg-core")
    if not os.path.isfile(mdbg_core) or not os.access(mdbg_core, os.X_OK):
        raise RuntimeError(f"mdbg-core executable is unavailable: {mdbg_core}")

    process = subprocess.Popen([path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    status = process.wait()
    if status != -signal.SIGSEGV:
        raise RuntimeError(
            f"cross-file inline fixture did not terminate with SIGSEGV: {status}"
        )
    core_path = f"/tmp/mdbg-core-{process.pid}"
    if not os.path.exists(core_path):
        raise RuntimeError("cross-file inline fixture did not produce a genuine core")

    try:
        output = subprocess.check_output(
            [mdbg_core, core_path],
            input=(
                "print shadow_value\n"
                "inline\n"
                "inline 1\n"
                "locals\n"
                "print shadow_value\n"
                "quit\n"
            ),
            text=True,
            stderr=subprocess.STDOUT,
        )
    finally:
        try:
            os.remove(core_path)
        except FileNotFoundError:
            pass

    outer_line = next(
        (line for line in output.splitlines() if "!inline_outer called at " in line),
        None,
    )
    inner_line = next(
        (line for line in output.splitlines() if "!inline_inner called at " in line),
        None,
    )
    if outer_line is None or "inline_core_fixture.c:" not in outer_line:
        raise RuntimeError(
            "mdbg-core did not resolve inline_outer DW_AT_call_file to the source file"
        )
    if inner_line is None or "inline_core_fixture.h:" not in inner_line:
        raise RuntimeError(
            "mdbg-core did not resolve inline_inner DW_AT_call_file to the header file"
        )

    selected = output.find("selected inline 1")
    if selected == -1:
        raise RuntimeError("mdbg-core did not select the compiler-proven inner inline context")
    selected_output = output[selected:]
    if "variable shadow_value" not in selected_output:
        raise RuntimeError(
            "selected inner inline context did not expose compiler-owned shadow_value"
        )

    scalar_marker = "!shadow_value = 0x141 [4-byte signed]"
    before_selection = output[:selected]
    if scalar_marker not in before_selection:
        raise RuntimeError(
            "existing snapshot evaluator could not materialize compiler-produced inner "
            "shadow_value from the immutable physical frame"
        )
    if scalar_marker not in selected_output:
        raise RuntimeError(
            "selected inline context could not materialize its compiler-produced "
            "shadow_value through mdbg-core print"
        )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_inline_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_inline_crash_probe")

    expected = {"inline_outer": None, "inline_inner": None}
    for record in records:
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        name = origin_name(record, by_offset)
        if name not in expected:
            continue
        call_file = record["attrs"].get("call_file")
        call_line = record["attrs"].get("call_line")
        if not call_file or not call_line:
            continue
        file_index = numeric_attr(call_file, f"{name}: call_file")
        line = numeric_attr(call_line, f"{name}: call_line")
        if file_index == 0 or line == 0:
            raise RuntimeError(f"{name}: zero call-site coordinate")
        has_low_high = "low_pc" in record["attrs"] and "high_pc" in record["attrs"]
        has_ranges = "ranges" in record["attrs"]
        if not has_low_high and not has_ranges:
            continue
        evidence = "low/high" if has_low_high else "ranges"
        expected[name] = (record["depth"], file_index, line, evidence)

    missing = [name for name, evidence in expected.items() if evidence is None]
    if missing:
        raise RuntimeError(f"missing concrete inline call-site metadata: {missing}")

    contexts = addr2line_contexts(path, probe)
    wanted = ["inline_inner", "inline_outer", "physical_frame"]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected addr2line inline chain: {chain}")

    outer = expected["inline_outer"]
    inner = expected["inline_inner"]
    if inner[0] <= outer[0]:
        raise RuntimeError("inline_inner DIE is not nested below inline_outer")
    if inner[1] == outer[1]:
        raise RuntimeError(
            "cross-file fixture did not produce distinct compiler DW_AT_call_file indices"
        )

    source_basenames = {
        location_basename(location) for _, location in contexts[: len(wanted)]
    }
    required_sources = {"inline_core_fixture.h", "inline_core_fixture.c"}
    if not required_sources.issubset(source_basenames):
        raise RuntimeError(
            "addr2line did not prove header/source inline ownership: "
            + ", ".join(sorted(source_basenames))
        )

    require_mdbg_core_callsite_ownership(path)

    for name in ("inline_outer", "inline_inner"):
        depth, file_index, line, evidence = expected[name]
        print(
            f"{name}: depth={depth} probe=0x{probe:x} range={evidence} "
            f"call_file={file_index} call_line={line}"
        )
    print(
        "addr2line chain: "
        + " -> ".join(
            f"{name}@{location}" for name, location in contexts[: len(wanted)]
        )
    )
    print("inline scalar: shadow_value=0x141 via existing snapshot evaluator")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"inline DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
