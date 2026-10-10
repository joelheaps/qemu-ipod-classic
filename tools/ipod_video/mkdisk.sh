#!/bin/sh
# Build a disk image for the iPod-Video machine.
#
# The result has the same layout as a real iPod: an MBR, then the firmware
# partition (type 0, which is why Windows never shows it), then the FAT32
# partition the firmware looks for its files on.
#
# The firmware half comes from the dump of the real device; the FAT32 half is
# built fresh here and filled with whatever is in --files, normally the
# .rockbox directory off the iPod.
#
#   mkdisk.sh <fw-dump> <files-dir> <output> [size-mb]
#
# Needs dosfstools and mtools:  sudo apt install dosfstools mtools

set -e

fw=${1:?falta el volcado de la particion de firmware}
files=${2:?falta el directorio a copiar en la FAT32}
out=${3:?falta el fichero de salida}
size_mb=${4:-256}

[ -f "$fw" ] || { echo "no existe $fw" >&2; exit 1; }
[ -d "$files" ] || { echo "no existe $files" >&2; exit 1; }

fat="$out.fat32.tmp"
sector=512

echo "1/4  creando FAT32 de ${size_mb} MB"
rm -f "$fat"
truncate -s "${size_mb}M" "$fat"
mkfs.vfat -F 32 -n IPOD "$fat" > /dev/null

echo "2/4  copiando $files"
# mtools wants a drive letter; point one at the bare image with no partition.
export MTOOLSRC=$(mktemp)
printf 'drive z: file="%s" partition=0\n' "$(readlink -f "$fat")" > "$MTOOLSRC"
mcopy -s -Q -i "$fat" "$files" ::/
rm -f "$MTOOLSRC"

echo "3/4  concatenando MBR + firmware + FAT32"
cat "$fw" "$fat" > "$out"
rm -f "$fat"

echo "4/4  ajustando la tabla de particiones"
# The dump already carries a valid MBR whose second entry describes a FAT32
# partition far larger than the one we just built, so rewrite its length. The
# entry is at 0x1be + 16, and the sector count is the last four bytes of it.
python3 - "$out" "$fw" "$sector" <<'EOF'
import struct, sys, os
path, fw, sector = sys.argv[1], sys.argv[2], int(sys.argv[3])
fw_bytes = os.path.getsize(fw)
total = os.path.getsize(path)
with open(path, 'r+b') as f:
    f.seek(0x1be + 16)
    entry = bytearray(f.read(16))
    start, = struct.unpack('<I', entry[8:12])
    if start * sector != fw_bytes:
        sys.exit("la particion FAT32 empieza en el sector %d (0x%x bytes) pero "
                 "el volcado mide 0x%x" % (start, start * sector, fw_bytes))
    count = (total - fw_bytes) // sector
    entry[12:16] = struct.pack('<I', count)
    f.seek(0x1be + 16)
    f.write(entry)
print("   FAT32: sector %d, %d sectores (%.1f MB)"
      % (start, count, count * sector / (1024 * 1024)))
EOF

echo
ls -la "$out"
