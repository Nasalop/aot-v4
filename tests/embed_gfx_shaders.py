import pathlib
import sys


def emit_array(name, data):
    rows = [data[i:i + 16] for i in range(0, len(data), 16)]
    body = ",\n".join("  " + ", ".join(f"0x{value:02x}" for value in row) for row in rows)
    return f"static const unsigned char {name}[] = {{\n{body}\n}};\n"


def main():
    if len(sys.argv) not in (4, 6):
        raise SystemExit("usage: embed_gfx_shaders.py VS.cso PS.cso output.h [VS_SYMBOL PS_SYMBOL]")

    vertex = pathlib.Path(sys.argv[1]).read_bytes()
    pixel = pathlib.Path(sys.argv[2]).read_bytes()
    vertex_symbol, pixel_symbol = (
        (sys.argv[4], sys.argv[5]) if len(sys.argv) == 6 else ("aot_stress_vs", "aot_stress_ps")
    )
    for label, blob in (("vertex", vertex), ("pixel", pixel)):
        if len(blob) < 32 or blob[:4] != b"DXBC":
            raise SystemExit(f"{label} shader is not a valid DXBC container")

    output = (
        "#pragma once\n"
        + emit_array(vertex_symbol, vertex)
        + f"static const unsigned long long {vertex_symbol}_size = sizeof({vertex_symbol});\n"
        + emit_array(pixel_symbol, pixel)
        + f"static const unsigned long long {pixel_symbol}_size = sizeof({pixel_symbol});\n"
    )
    pathlib.Path(sys.argv[3]).write_text(output, encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
