# snapshot-fix: `-loadvm` aborts with `assert(first_elem || !n_elems || !size)`

status: done   owner: snapshot-fix   updated: 2026-10-11

## Goal

Make the `save once, restore in seconds` loop usable for `-M iPod-Classic`: `savevm` a qcow2
overlay, then restore it in a fresh QEMU with `-loadvm`. The observed failure was

    qemu-system-arm: ../migration/vmstate.c:140: vmstate_load_state: Assertion
    `first_elem || !n_elems || !size' failed.

after `savevm np` had succeeded and `info snapshots` listed the snapshot.

## Reproduction

Save (this is the run that creates the snapshot; note `-icount`):

    FWGUID=000A270021574219 IRAMSPILL=0x10000 \
    build/qemu-system-arm -M iPod-Classic \
      -kernel assets/firmware/2.0.5/osos_decrypted.bin -serial null \
      -drive if=ide,format=qcow2,file=$OVERLAY \
      -icount shift=auto,sleep=off -display none -L build/pc-bios \
      -monitor unix:$MON,server,nowait
    # wait ~30 s, then in the monitor:
    (qemu) savevm np
    (qemu) info snapshots
    List of snapshots present on all disks:
    ID        TAG               VM SIZE                DATE     VM CLOCK     ICOUNT
    --        np               23.5 MiB 2026-10-11 20:55:38 00:32:44.331

Restore **without** `-icount` (the failing command):

    FWGUID=000A270021574219 IRAMSPILL=0x10000 \
    build/qemu-system-arm -M iPod-Classic \
      -kernel assets/firmware/2.0.5/osos_decrypted.bin -serial null \
      -drive if=ide,format=qcow2,file=$OVERLAY \
      -display none -L build/pc-bios -loadvm np

The assert fires because the snapshot was taken with `-icount` but the restoring process was
not. The `-icount` *value* does not matter (`shift=auto,sleep=off`, `shift=7`, ...); what matters
is whether the icount clock is enabled at all. The reverse mismatch (save without `-icount`,
load with `-icount`) is harmless: the stream simply has no icount subsection.

## Root cause

`-icount` creates the per-CPU icount timers and describes them in a vmstate subsection
(`system/cpu-timers.c`):

    icount_vmstate_timers          "timer/icount"        (needed: icount_enabled())
      icount_vmstate_warp_timer    "timer/icount/warp_timer"
      icount_vmstate_adjust_timers "timer/icount/timers"
        VMSTATE_TIMER_PTR(icount_rt_timer, TimersState)
        VMSTATE_TIMER_PTR(icount_vm_timer, TimersState)
      icount_vmstate_shift         "timer/icount/shift"

The save path honours each subsection's `needed` predicate, so with `-icount` on the
`timer/icount/timers` subsection is written. The **load path does not**:
`vmstate_subsection_load()` (`migration/vmstate.c`) matches the subsection name in the stream and
calls `vmstate_load_state()` directly, without consulting `sub_vmsd->needed`. In the fresh
process started without `-icount`, `timers_state.icount_rt_timer` and `icount_vm_timer` are
NULL, so loading their `VMSTATE_TIMER_PTR` fields trips the pointer assert at
`migration/vmstate.c:140`.

`qemu-system-arm -trace enable=vmstate_load_state` on the failing restore names the culprit:

    vmstate_load_state timer/icount v1
    vmstate_load_state timer/icount/timers v1
    qemu-system-arm: ../migration/vmstate.c:140: vmstate_load_state: Assertion `first_elem || !n_elems || !size' failed.

This is upstream QEMU behaviour, not something the iPod device models introduced: `needed` is
implicitly a symmetric predicate (the same configuration is expected at both ends). A subsection
has no length field in the stream, so an unneeded-but-present subsection cannot simply be skipped
generically. It can, however, be consumed field by field when the only difference is that the
local pointer is NULL.

## Fix

Optional timer pointers are now explicitly allowed to be NULL on load, and the saved expiry is
consumed and discarded instead of aborting:

- `include/migration/vmstate.h`: new `VMS_NULLABLE_POINTER` field flag and
  `VMSTATE_POINTER_NULLABLE`; `VMSTATE_TIMER_PTR_V` (used by `VMSTATE_TIMER_PTR`) now sets the
  flag.
- `migration/vmstate.c`: a `VMS_NULLABLE_POINTER` field whose local pointer is NULL does not
  assert and its `info->get()` is called with a NULL pointer to consume the stream bytes.
- `migration/savevm.c`: `get_timer()` reads and discards the 8-byte expiry when handed a NULL
  timer, instead of dereferencing it.

The change only affects loading a NULL single timer pointer. Save, the normal (matching) load
path, and every non-timer field are untouched, so the default boot path is unaffected. The
matching-`-icount` restore (the supported loop) is byte-for-byte the same as before.

## Evidence

Pre-fix, the mismatched restore aborted (recorded before the fix):

    $ ... -loadvm np            # no -icount
    IRAMSPILL: 65536 bytes mapped above IRAM1 (measurement aid, not a fix)
    qemu-system-arm: info: loaded 0xa24970 raw bytes at 0x8000000
    SM1_IRQ: engine interrupt armed on VIC0 line 18
    VDEC: 0x39800000 stub device up
    SM1_DECODE: frame decoder on (libavcodec)
    qemu-system-arm: ../migration/vmstate.c:140: vmstate_load_state: Assertion `first_elem || !n_elems || !size' failed.
    Aborted (core dumped)          # exit 134

Post-fix, the same mismatched restore loads and the guest resumes (`verify.py`, which starts QEMU
paused, screendumps, then `cont`s and checks the run state):

    status(before cont): VM status: paused (prelaunch)
    lit(loaded) = 76796             # restored 320x240 frame, non-black
    cont
    status(after cont): VM status: running
    qemu exit: 0

The matching-config restore and the no-`-icount` restore still work:

    save -icount, load -icount : lit=76796, status running, exit 0
    save no-icount, load no-icount : lit=75463, status running, exit 0

`tests/unit/test-vmstate` (the vmstate unit test) passes:

    $ build/tests/unit/test-vmstate
    ...
    ok 23 /vmstate/qlist/load/loadqlist
    # End of vmstate tests

## Friction

- The brief said the abort happens with "the same command line + env". It does not: the same
  configuration saves and restores fine. The abort needs `-icount` at save and its absence at
  load. The asymmetry is easy to miss because both runs otherwise look identical, and the assert
  names no device (only `vmstate_load_state`), so `-trace enable=vmstate_load_state` was needed to
  find `timer/icount/timers`.
- `migration/vmstate.c` cannot reference `vmstate_info_timer` directly: that symbol lives in
  `migration/savevm.c`, and `tests/unit/test-vmstate` links `vmstate.c` without `savevm.c`, so the
  unit test fails to link. That is why the fix uses a new `VMStateField` flag rather than comparing
  `field->info` against the timer info.
- QEMU's subsection loader ignoring `needed` is a general migration limitation. Other
  configuration-dependent subsections with pointer fields can hit the same assert; the same
  NULL-pointer-consume pattern would be needed for them.
