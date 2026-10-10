# assets/disks/

QEMU IDE disk images laid out like the iPod's HDD.

| File | Use |
|---|---|
| `Emedia.img` | boot disk with the music DB the guest accepts (`DISK=` default for rehearsals) |
| `Emedia0.img` | pristine copy of `Emedia.img` |
| `Emedia-fixed.img` | not generated: needs the 1036 missing tracks (see below) |
| `E.img` | firmware partition + empty data FAT, no music |
| `fat32.img` | the data FAT used to build `E.img` |

Built by external disk-image tooling (not in this tree) from the firmware and a media directory. Regenerable: yes. Back up: no.
Run with `-snapshot` (or `SNAPSHOT=1` in rehearsals) to keep them clean.

Consistency: `Emedia.img` carries the device's full iTunesDB (1086 tracks: 714 `.m4a`, 372 `.m4v`) but only 50
track files (`assets/device/media` holds just those). 1036 referenced files are absent, e.g.
`Music/F07/libgpod292653.m4a`, so the guest's codec open returns -1.
A consistent `Emedia-fixed.img` needs the
missing files copied into `assets/device/media/iPod_Control/Music/` (from the device) or a
re-signed pruned iTunesDB (hash58 over the GUID; not implemented).
