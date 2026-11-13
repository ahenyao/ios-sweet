
#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])

data = src.read_bytes()

if len(data) < 64:
    raise SystemExit("File is too small")

magic, total_size = struct.unpack_from(">II", data, 0)

if magic != 0xD7B7AB1E:
    raise SystemExit(f"Invalid Samsung DTB magic: {magic:#x}")

fdt_size, fdt_offset = struct.unpack_from(">II", data, 0x20)

if fdt_offset + fdt_size > len(data):
    raise SystemExit("Embedded FDT exceeds container size")

fdt = data[fdt_offset:fdt_offset + fdt_size]

if fdt[:4] != b"\xd0\x0d\xfe\xed":
    raise SystemExit("Invalid FDT magic")

actual_fdt_size = struct.unpack_from(">I", fdt, 4)[0]

if actual_fdt_size != len(fdt):
    raise SystemExit("FDT size mismatch")

dst.write_bytes(fdt)

print(f"Container: {len(data)} bytes")
print(f"FDT offset: {fdt_offset:#x}")
print(f"FDT size: {fdt_size} bytes")
print(f"Extracted: {dst}")
