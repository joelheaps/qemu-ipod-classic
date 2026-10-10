#!/usr/bin/env python3
"""Find where ARM code actually loads a given constant from its literal pool.

Searching a firmware image for the bytes of an address finds mostly noise:
0x40000000 is also the high word of the double 2.0, so a raw byte search turns
up floating-point tables and calls them pointers. This instead decodes every
`ldr rD, [pc, #imm]` in the image, works out which word it loads, and reports
only the ones that load the value we asked about. Those are references the code
really makes.

    python findlit.py <image> <hex-value> [<hex-value> ...]
"""

import struct
import sys


def pc_relative_loads(data):
    """Yield (instruction_offset, literal_offset, value) for every ldr rD,[pc,#imm]."""
    for off in range(0, len(data) - 3, 4):
        insn = struct.unpack_from("<I", data, off)[0]

        # cond 0101 U0011111 Rd imm12 -- ldr rD, [pc, +/-imm]
        if (insn & 0x0F7F0000) != 0x051F0000:
            continue
        # Skip never-executed conditions rather than reporting them as uses.
        if (insn >> 28) == 0xF:
            continue

        imm = insn & 0xFFF
        up = (insn >> 23) & 1
        # The PC reads as the instruction address plus eight.
        lit = off + 8 + imm if up else off + 8 - imm
        if 0 <= lit <= len(data) - 4:
            yield off, lit, struct.unpack_from("<I", data, lit)[0]


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1

    data = open(argv[1], "rb").read()
    wanted = {int(a, 16) for a in argv[2:]}

    print("image: %s (0x%x bytes)" % (argv[1], len(data)))
    total = 0
    found = {v: [] for v in wanted}
    for off, lit, value in pc_relative_loads(data):
        total += 1
        if value in wanted:
            found[value].append((off, lit))

    print("pc-relative loads decoded: %d" % total)
    for value in sorted(wanted):
        hits = found[value]
        print("\n0x%08x -- loaded by %d instruction(s)" % (value, len(hits)))
        for off, lit in hits[:20]:
            print("    ldr at 0x%06x, literal at 0x%06x" % (off, lit))
        if len(hits) > 20:
            print("    ... and %d more" % (len(hits) - 20))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
