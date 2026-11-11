#!/usr/bin/env python3
"""Compare two binary PPM (P6) images.

    compare_ppm.py A.ppm B.ppm [--tolerance N] [--max-fraction F]

A byte "differs" when |a - b| > tolerance (default 2 of 255). The images match
when at most max-fraction of the bytes differ (default 0.002). Prints one line
and exits 0 on a match, 1 on a mismatch, 2 when a file cannot be compared.
No dependencies.
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # magic, width, height, maxval, each followed by whitespace; '#' comments allowed
    tokens, pos = [], 0
    while len(tokens) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        tokens.append(data[pos:end])
        pos = end
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError("%s: not an 8-bit binary PPM" % path)
    width, height = int(tokens[1]), int(tokens[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError("%s: truncated" % path)
    return width, height, pixels


def main(argv):
    args, tolerance, max_fraction = [], 2, 0.002
    i = 1
    while i < len(argv):
        if argv[i] == "--tolerance":
            tolerance = int(argv[i + 1]); i += 2
        elif argv[i] == "--max-fraction":
            max_fraction = float(argv[i + 1]); i += 2
        else:
            args.append(argv[i]); i += 1
    if len(args) != 2:
        print(__doc__)
        return 2
    try:
        wa, ha, a = read_ppm(args[0])
        wb, hb, b = read_ppm(args[1])
    except (OSError, ValueError, IndexError) as e:
        print("cannot compare: %s" % e)
        return 2
    if (wa, ha) != (wb, hb):
        print("size differs: %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    differ, worst = 0, 0
    for x, y in zip(a, b):
        d = x - y if x > y else y - x
        if d > tolerance:
            differ += 1
        if d > worst:
            worst = d
    fraction = differ / len(a)
    ok = fraction <= max_fraction
    print("%s  bytes off by more than %d: %d of %d (%.4f %%, limit %.2f %%), largest difference %d"
          % ("match   " if ok else "MISMATCH", tolerance, differ, len(a), fraction * 100.0,
             max_fraction * 100.0, worst))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
