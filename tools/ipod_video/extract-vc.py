#!/usr/bin/env python3
"""Extract the VideoCore files from the iPod firmware partition.

The firmware partition of assets/ipod-video-disk.img carries a small FAT16
volume labelled IPODRESOURC (boot sector at file offset 0x74EC00).  Inside it,
/Resources/VideoCore/Boot/vmcs.bin is the firmware the retailOS uploads to the
Broadcom BCM2722, and /Resources/VideoCore/Library/*.vll are the loadable
VideoCore ELF modules (e_machine = 95, EM_VIDEOCORE).

  python3 tools/ipod_video/extract-vc.py assets/ipod-video-disk.img [outdir]

Everything below is derived from the volume's own BPB and directory entries;
nothing is hardcoded except the offset of the boot sector.
"""
import os
import struct
import sys

FS_OFF = 0x74EC00  # boot sector of the IPODRESOURC FAT16 volume


def main():
    img_path = sys.argv[1] if len(sys.argv) > 1 else 'assets/ipod-video-disk.img'
    outdir = sys.argv[2] if len(sys.argv) > 2 else 'assets/images-5g'
    img = open(img_path, 'rb')

    img.seek(FS_OFF)
    bs = img.read(512)
    if bs[54:62] != b'FAT16   ':
        sys.exit('no FAT16 boot sector at 0x%X' % FS_OFF)
    bps, spc = struct.unpack_from('<HB', bs, 11)
    rsvd, nfat, rootent = struct.unpack_from('<HBH', bs, 14)
    fatsz, = struct.unpack_from('<H', bs, 22)
    rootsec = (rootent * 32 + bps - 1) // bps
    first_data = rsvd + nfat * fatsz + rootsec

    def clus_off(n):
        return FS_OFF + (first_data + (n - 2) * spc) * bps

    def read_dir(off, size):
        img.seek(off)
        return img.read(size)

    # The volume is laid out contiguously (verified: every file's first cluster
    # equals the previous file's first cluster plus its rounded-up length), so
    # a file is just <first cluster offset> .. + <size>.
    def walk(entries, path, out):
        lfn = {}
        for i in range(0, len(entries), 32):
            e = entries[i:i + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}
                continue
            if e[11] == 0x0F:
                lfn[e[0] & 0x1F] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16-le', 'replace')
                continue
            short = e[0:8].decode('latin1').rstrip()
            ext = e[8:11].decode('latin1').rstrip()
            name = short + ('.' + ext if ext else '')
            if lfn:
                name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0].split('￿')[0]
            lfn = {}
            if short in ('.', '..'):
                continue
            clus, = struct.unpack_from('<H', e, 26)
            size, = struct.unpack_from('<I', e, 28)
            full = path + '/' + name
            if e[11] & 0x10:
                nbytes = spc * bps
                walk(read_dir(clus_off(clus), nbytes), full, out)
            else:
                out.append((full, clus_off(clus), size))

    files = []
    walk(read_dir(FS_OFF + (rsvd + nfat * fatsz) * bps, rootent * 32), '', files)

    wanted = {
        '/Resources/VideoCore/Boot/vmcs.bin': 'vmcs.bin',
        '/Resources/VideoCore/Boot/RenderServer.bin': 'videocore/RenderServer.bin',
    }
    os.makedirs(os.path.join(outdir, 'videocore'), exist_ok=True)
    for full, off, size in files:
        print('%-48s off=0x%08X size=%d' % (full, off, size))
        dest = wanted.get(full)
        if dest is None and full.startswith('/Resources/VideoCore/Library/'):
            dest = 'videocore/' + full.rsplit('/', 1)[1]
        if dest is None:
            continue
        img.seek(off)
        with open(os.path.join(outdir, dest), 'wb') as f:
            f.write(img.read(size))
        print('    -> %s' % os.path.join(outdir, dest))


if __name__ == '__main__':
    main()
