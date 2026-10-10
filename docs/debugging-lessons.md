# Debugging lessons from bringing up both machines

Rules that held across both ports, each with the case that taught it. Workflow and tooling (ledgers, snapshots,
pctrace): `../osos-re/docs/experiments.md`. Guest debugging recipes: `hw/arm/ipod_classic/README.md`.

## Failure patterns

| Pattern | What it looks like | Cases |
|---|---|---|
| A status bit that never arrives | Silent infinite loop: no log, no fault, emulator "alive" | 5G: `PLL_STATUS` 31, cache `READY`, USB `+0x28` 7, BCM ready bits, PCF50605 `ADCRDY`. 6G: `PLLLOCK`, RNG ready, I2C byte-done |
| Zero is not a harmless unknown | A third answer the firmware has no case for | `PROCESSOR_ID` 0 is neither core; `PP_VER` 0 picks another chip's IRAM base; active-low GPIO 0 = every button pressed; wheel data 0 = "no packet" |
| An unmapped register is a data abort | Crash in a handler that itself faults | Put an `-d unimp` floor under every register window, models on top |
| Skipping the boot ROM is a debt | Firmware reads state nobody wrote | 5G SysInfo `{IsyS, ptr}` and `GPO32` bit 14; 6G boot info block (`HwVr` 19) |
| A raised line is not a delivered event | Device IRQs fire, guest does nothing | 5G `HI_IRQ` summary bit gates the high bank; 6G hold switch read gates buttons but not wheel turns. Check the path per event type |
| A readback of 0 breaks save/restore | Guest masks itself | 6G `WHEELMASK` read as 0, so restoring it wrote 0 |
| The guest's own panic output was ignored | Black screen read as "stuck" | Semihosting printed `_sys_exit` / `CPU_exception_handler` the whole time; one line to enable |

## Evidence rules

1. **A negative only rules out what would have shown in the state you measured.** `0x38900000` was dismissed
   because "no register gets a DRAM address": true, because the volume was not mounted yet. It was the
   compositor. Write dismissals with their condition: "no source address *on a diskless boot*".
2. **Prove the instrument with a known positive first.** `grep -c '^0xADDR'` on `-d in_asm` only matches TB
   heads; a hot address returned 0. pctrace has the same mid-TB caveat.
3. **Count values, not accesses.** 793 distinct writes to `0x38900000` included an identity gamma table and the
   panel geometry, signatures only a video path has. "245 references to the DSP at `0x3F000000`" were the float
   `0.5f`.
4. **Positive signatures outweigh absences.** A signature with one explanation beats an absence with several.
5. **The obvious suspect is often just the last block you touched.** The ATA was blamed for the disk-boot hang;
   it had finished cleanly (`ATA_CFG = 0x640`, then silence). The hang was a later `memcpy` over DMAC0 waiting
   on IRQ 16.
6. **Something the firmware compares but never produces came from outside.** `"IsyS"` is loaded by one
   instruction in 7 MB, for a compare: the ROM wrote it.
7. **Decode, don't byte-search.** `tools/ipod_video/findlit.py` decodes `ldr rD, [pc, #imm]`; a byte search for `0x40000000`
   gave 139 hits (mostly the double `2.0`), the decode gave 1.
8. **Look at the pixels.** `pmemsave` of a layer buffer showed the whole language screen already composed in
   DRAM, and later the main menu drawn under a stale layer. A frame is not a state: capture at intervals.
9. **Prefer verified reference over comments.** Comments and docs went stale under code changes several times
   (a header that contradicted the function 30 lines below). Mark guesses `ASSUMED` / `INFERRED` /
   `UNDETERMINED` next to the value, and do not fill a struct from a different firmware's layout ("inventing
   with the appearance of rigour").

## Techniques, by yield

| Technique | Use |
|---|---|
| Semihosting on, read stderr | The firmware's own panic text and register dump |
| gdb breakpoint + write watchpoint | Who writes a field, with what (`set arm abi AAPCS` first) |
| Per-device `*TRACE=1` with guest PC and values | `DISPTRACE`, `LCDTRACE`, `ATATRACE`, `WHEELTRACE`, `GPIOTRACE`; totals must count everything |
| pctrace plugin | PCs, registers and memory without stopping the guest |
| `pmemsave` + view as image | Framebuffers before a display path exists |
| Read the guest's own state block | e.g. the wheel ISR's decoded event; proves the event was drained, not just raised |
| Control cases | A bare-metal test guest (`tools/ipod_classic/tests/`); a pixel-diff noise floor of 0 on a clockless screen |
| Pixel-diff thresholds | One highlight step ≈ 8 000 px, a new screen ≈ 50 000 px; a few px is the status-bar clock |
| `-d unimp` caveat | In a tight loop it slows the guest enough to look hung; DMA transfers emit no `ide_sector_read` |
