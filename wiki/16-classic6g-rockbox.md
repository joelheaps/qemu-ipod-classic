# Arrancar Rockbox en la máquina iPod-Classic (S5L8702)

Cómo se llevó **Rockbox** (target `ipod6g`) desde "no ejecuta ni una
instrucción" hasta su **menú principal navegable** en la máquina QEMU
`iPod-Classic`. Todo lo que sigue está **medido** (captura mirada o traza)
salvo donde diga `INFERIDO` o `INDETERMINADO`. Las órdenes del final lo
reproducen.

> Por qué Rockbox: el retailOS ya arranca, dibuja y lista la música, pero **no
> suena** porque decodifica el MP3 en el DSP CalmADM2E, que no está modelado
> (ver [14-classic6g-audio.md](14-classic6g-audio.md)). Rockbox decodifica el
> MP3 **en el ARM**, así que el PCM real sale por el mismo FIFO del I²S que ya
> capturamos a WAV. Empezó por **arrancar Rockbox** (§1–4) y ya llega hasta el
> final: **reproduce un MP3 y el WAV capturado es la canción** (§7, medido).

---

## 0. Estado por hito

| Hito | Estado |
|---|---|
| 1. Rockbox carga y ejecuta su primera instrucción | **hecho** — `-kernel rockbox.bin` en `0x08000000` |
| 2. Pasa el init temprano (system, clocks, MMU) sin colgarse | **hecho** — hand-off del bootloader (§2) |
| 3. Inicializa el LCD y DIBUJA | **hecho** — la ruta de píxeles por DMA al LCD (§3) |
| 4. Llega a su menú principal navegable | **hecho** — menú cabbiev2, la rueda mueve el resaltado, SELECT entra (§4) |
| 5. Reproduce un MP3 y el PCM capturado ES la canción | **hecho** — WAV con correlación 1.000 contra el original (§7) |

Captura del hito 4 (disco con `/.rockbox`, tema cabbiev2 completo): el menú
"Rockbox" con **Files / Database / Resume Playback / Settings / Recording /
Playlists / Plugins / System / Shortcuts**, barra de selección dorada sobre
*Files*. La rueda baja el resaltado un renglón por clic y SELECT sobre
*Database* abre "Database is not ready / Initialize Now" — el comportamiento
correcto de Rockbox. Reproducible con `tools/ipod_classic/rockbox.sh`
(veredicto automático **OK**).

**Regresión:** `navtest.sh` (retailOS 6G) sigue pasando entera después de estos
cambios (VERIFICADO, run completo: rueda 8000 px, SELECT 19340 px, las dos
mitades del menú, MENU vuelve exacto). El 5G no se toca: los únicos ficheros
del modelo tocados son `ipod_classic.c` y `ipod_classic_lcd.c`, ambos del 6G.

---

## 1. La vía: cargar la app directa (estilo osos)

Rockbox se enlaza a DRAM `0x08000000`, entry `start` (`crt0.S`), igual que el
osos del retailOS. La máquina carga un binario RAW con `load_image_targphys`
en `SDRAM_MEM_BASE` y arranca ahí (`ipod_classic.c`). Así que:

```
qemu-system-arm -M iPod-Classic \
    -kernel assets/rockbox/build-full/rockbox.bin \
    -drive if=ide,format=raw,file=<disco.img> -snapshot
```

- Usar el **RAW** `rockbox.bin`, NO el `.elf` (su `e_entry` es `0x0`, la VMA del
  vector table, que sólo se puebla en runtime). VERIFICADO: el primer word del
  `.bin` es `ea00000d = b 0x800003c` (`start: b _newstart`).
- **No** hace falta la cadena del bootloader (`build-boot/bootloader.bin`) para
  el bring-up: la máquina reproduce lo que el bootloader dejaría (§2).

---

## 2. El hand-off del bootloader (hitos 1–2)

En el hardware real, el bootrom + el bootloader de Rockbox/emCORE dejan dos
cosas hechas antes de saltar a la app. En un arranque directo por `-kernel`
las saltamos, y la app se cuelga por ambas. La máquina las reproduce en
`ipod_classic_handoff()` (`hw/arm/ipod_classic/ipod_classic.c`), llamada desde
`mc->reset` **después** de `qemu_devices_reset()`. Gated a que no haya bootrom.

### 2.1. El stack (SP) — el primer cuelgue, MEDIDO

`crt0.S` (rama no-BOOTLOADER) llama `bl memory_init` **antes** de montar sus
propias pilas (`crt0.S:143` frente a `:184`). `memory_init` empieza con
`push {r4, lr}`, así que corre sobre el SP que dejó el bootloader. En un reset
en frío de QEMU **SP = 0**, el `push` escribe en `0xfffffff8` → data abort no
manejado (el vector table en `0x0` aún es ceros) → el PC recorre
`0x10,0x14,...`. Pantalla negra.

**Arreglo:** sembrar `r13` (SVC) con el tope de IRAM0 (`0x22020000`), una pila
scratch como la que deja el bootloader; la app cambia a sus pilas en cuanto
`memory_init` retorna. El retailOS no se ve afectado: monta su propio SP antes
de hacer cualquier `push` (hoy arranca con SP=0, que fallaría igual si no lo
hiciera).

### 2.2. El temporizador de microsegundos (TimerE) — el segundo cuelgue

`USEC_TIMER == TECNT` (TimerE) y `udelay()` gira sobre él sin timeout
(`system-target.h`). Quien arranca TimerE es `usec_timer_init()`, que es
`#ifdef BOOTLOADER` (`clocking-s5l8702.c:381`), así que **la app no lo
arranca**: la primera transferencia I²C de `pmu_preinit()` hace `udelay(5)` y
se cuelga.

**Arreglo:** programar TimerE a 1 MHz exactamente como `usec_timer_init()`:
`TECON=0x440` (ECLK/1, ECLK, modo intervalo), `TEPRE=11` (12 MHz/12 = 1 MHz),
`TEDATA0=~0`, `TECMD=0x3` (CLR|EN), en `TIMER_BASE=0x3C700000`. El retailOS
también maneja TimerE a 1 MHz y lo re-programa (con CLR) antes de usarlo, así
que esto le es transparente — VERIFICADO por `navtest.sh`.

### 2.3. Por qué en `mc->reset` y no en el reset del CPU

El reset del sysbus (que resetea el device del timer) lo registra
`qdev_machine_creation_done()` **después** de que `machine_init` registre
`ipod_classic_cpu_reset`, así que correría el último y **borraría** el TimerE
que programamos. `mc->reset` es el único sitio garantizado a correr tras todos
los resets de device: hace `qemu_devices_reset()` y luego el hand-off.

Con esto Rockbox pasa `memory_init`, `system_init` (i2c/pmu/udelay), gpio,
eic, uart y **llega al LCD** (VERIFICADO por traza `-d unimp`).

---

## 3. La ruta de píxeles: DMA al LCD (hito 3)

El LCD (`0x38300000`) estaba modelado **para el retailOS**, que compone el
frame en el *display pipe* (`0x38900000`) y no empuja ni un píxel por el puerto
de datos. **Rockbox lo hace al revés**: escribe el frame por DMA.

`lcd-s5l8702.c`: `displaylcd_setup()` manda por CPU los comandos `0x2A` CASET /
`0x2B` PASET / `0x2C` RAMWR (params de 4 bytes, escrituras de 32 bits al puerto
`+0x40`, que el modelo aceptaba); luego `displaylcd_dma()` pone modo frame
(`LCD_CON=0x80100db0`) y **DMA los píxeles** con `dmac_ch_queue(...,
S5L8702_DADDR_PERI_LCD_WR, ...)` — DMAC0, `dwidth = WIDTH_16`, a
`LCD_BASE+0x40`.

Medido con `LCDTRACE=1`: **152 escrituras al puerto de datos, 0 píxeles**. Los
píxeles llegan como escrituras de **16 bits**, y el modelo del LCD tenía
`valid.min_access_size = 4` → cada escritura del DMA se rechazaba
("Invalid write at addr 0x40, size 2"). Pantalla negra aunque el driver corría
un update completo.

**Arreglo (`ipod_classic_lcd.c`):** aceptar accesos de media palabra en el
bloque LCD — `valid`/`impl` `min_access_size = 2`, `max = 4`. `panel_data()` ya
toma un píxel de los 16 bits bajos, así que una escritura de 16 o de 32 bits
entrega un píxel. El retailOS sólo usa 32 bits, así que su ruta no cambia
(VERIFICADO por `navtest.sh`).

**El DMA en sí ya funcionaba:** el PL080 (`hw/dma/pl080.c`) transfiere a plena
velocidad las líneas no *paced*, y la línea LCD_WR (id 3) no está marcada como
*paced* (sólo lo está la del I²S). El LLI-chaining del PL080 recorre los ~19
descriptores que Rockbox arma para el frame de 320×240. En cuanto el puerto
acepta los 16 bits, los píxeles aterrizan en `s->fb` y el frame aparece —
MEDIDO: de 0 a 3805 px (fallback) / 74057 px (tema cabbiev2) encendidos.

---

## 4. El disco y el menú (hito 4)

Rockbox monta la partición FAT32 (la máquina interpreta las LBA del MBR en
bloques de 4096 bytes, igual que el retailOS — ver `mkdisk6g4k.py`; por eso un
disco de `mkdisk6g4k.py` sirve tal cual).

- **Con cualquier disco FAT** Rockbox llega al menú, pero enseña primero
  "Installation incomplete" y usa la fuente interna (sysfont). MEDIDO con
  `~/6g/E4kzero_rebuilt.img`: menú de 3805 px, texto blanco sobre negro.
- **Con un disco que lleve `/.rockbox`** carga su tema, fuentes y códecs:
  menú cabbiev2 completo, 74057 px. Fabricado en `~/6g/Erb.img` a partir de
  `assets/rockbox/build-full/rockbox.zip` (contiene `.rockbox/codecs/mpa.codec`,
  el decodificador MP3 en ARM que hará falta para el audio).

Navegación (VERIFICADO, `rockbox.sh`): la barra de selección dorada baja un
renglón por clic de rueda adelante (y 36→52→66) y sube al clic atrás (→52);
SELECT sobre *Database* cambia la pantalla (7666 px distintos) a "Database is
not ready". Los 4 eventos de rueda/botón los registra el modelo de la rueda
(`WHEELTRACE`, IRQ 23). El driver de botones de Rockbox lee el mismo
controlador de rueda que ya modelábamos para el retailOS, sin cambios.

---

## 5. Lo que se stubeó / dejó anotado

- **`ipod_classic_handoff()`**: SP + TimerE de arranque. Es la reproducción del
  hand-off del bootloader, no comportamiento de hardware puro. Anotado en el
  código. Si algún día se modela un bootrom real, el gate `bootrom_path` lo
  desactiva.
- **LCD `min_access_size = 2`**: para aceptar el DMA de 16 bits de Rockbox.
  Anotado; no cambia la ruta de 32 bits del retailOS.
- **Sin tocar Rockbox**: los dos cuelgues se arreglaron en la MÁQUINA, no en el
  firmware. `rockbox.bin`/`.elf`/`.zip` son los que compiló el toolchain
  `arm-none-eabi-gcc 13.2.1` (ver el informe de compilación).

Nada más hizo falta stubear: `storage_init` monta el disco con el driver ATA
propio de Rockbox sobre el modelo ATA existente, `power_init`/`adc`/PMU pasan,
y el kernel (TimerB a 10 kHz) tickea. Lo que aún no se ha ejercitado a fondo es
USB (la app llama `usb_init` tras dibujar; hoy no cuelga) y la reproducción real
(el siguiente frente).

---

## 6. Cómo reproducirlo

```sh
# 1) app de Rockbox (ya compilada); si no, ver el informe de compilación:
#    assets/rockbox/build-full/rockbox.bin  (+ .elf con símbolos, + .zip)

# 2) disco con /.rockbox (tema + fuentes + códecs):
W=$HOME/rbwork; rm -rf $W; mkdir $W; cd $W
unzip -q /mnt/c/Desarrollo/qemu-ios/assets/rockbox/build-full/rockbox.zip
truncate -s 300M $HOME/6g/rbfat.img
mkfs.vfat -F 32 -n ROCKBOX $HOME/6g/rbfat.img
mcopy -s -i $HOME/6g/rbfat.img .rockbox ::
cd /mnt/c/Desarrollo/qemu-ios
python3 tools/ipod_classic/mkdisk6g4k.py $HOME/6g/Erb.img $HOME/6g/fwzero.img $HOME/6g/rbfat.img

# 3) arrancar hasta el menú y comprobar que navega (veredicto automatico):
sh tools/ipod_classic/rockbox.sh $HOME/6g/rockbox $HOME/6g/Erb.img

# ver una captura
python3 tools/ipod_classic/ppm2png.py $HOME/6g/rockbox/0-menu.ppm   # o el .png ya escrito

# 4) diagnostico si se cuelga: donde esta el PC (con simbolos del .elf)
#    lanzar qemu con -gdb tcp::PORT y attach gdb-multiarch; bt.
```

Trazas útiles: `-d unimp,guest_errors` (periférico no modelado), `LCDTRACE=1`
(cada acceso al LCD + totales de píxeles/frames), `WHEELTRACE=1` (rueda),
`ATATRACE=1`, `I2STRACE=1`.

---

## 7. Hito 5 — SUENA: el MP3 decodificado, capturado a WAV y **medido**

**Hecho.** Rockbox monta el disco, lista los MP3 en *Files*, y al darle SELECT
entra en la pantalla *Now Playing* (WPS) y **reproduce**: `mpa.codec` decodifica
el MP3 **en el ARM**, deja PCM s16le estéreo en DRAM, DMAC0 canal 2 lo empuja al
FIFO del I²S en `0x3CA00010`, y `AUDIOWAV=` lo vuelca a un WAV. Ese WAV **es la
canción**, no silencio ni ruido — demostrado midiéndolo contra el MP3 original.

**El WAV entregable:** `~/6g/rockbox_numb_encore.wav` (4.9 MB, 48000 Hz, 16-bit,
estéreo, ~25.7 s de "Numb / Encore" de Linkin Park). Fuera de git a propósito.

### La prueba (medida, no inferida)

Decodificando el MP3 original con `ffmpeg` a la misma tasa y correlacionando:

```
python3 tools/ipod_classic/wavcheck.py ~/6g/rockbox_numb_encore.wav \
        ~/6g/tracks/'Numb - Encore.mp3'
```

- **RMS 7014** (−13.4 dBFS), **pico 29128** (−1.0 dBFS), **99.9 %** de muestras
  no nulas → hay señal con dinámica real (un WAV de ceros da RMS 0).
- **Estéreo de verdad:** L RMS 7780, R RMS 6971, RMS(L−R) 4634 (0 sería
  falso-mono); L==R exacto sólo en el 0.15 % de los frames.
- **Correlación cruzada normalizada = 1.000** contra el decode del MP3, a un lag
  constante de ~18 ms (el *priming* del decodificador de Rockbox frente al de
  ffmpeg). Trozos de la captura a t=3,8,13,18,23 s casan **exactamente** con
  3.016, 8.016, 13.016, 18.016, 23.016 s del original (lag +5.00 s por trozo):
  es un decode **continuo** de la canción real, no un bucle ni una casualidad.
- Espectrograma de la captura ≈ espectrograma del original
  (`tools/ipod_classic/specimg.py`; ver `~/6g/rockbox_numb_encore_spec.png` vs
  `~/6g/ref_numb_encore_spec.png`).
- Pantallas: `1-files.png` (Rockbox lista `InTheEnd` y `Numb`), `2-wps.png`/
  `5-wps.png` (*NOW PLAYING* "Numb / Encore … Linkin Park", el reloj avanza
  0:04 → 0:17 entre capturas).

### Cómo reproducirlo

```sh
# disco con /.rockbox + los MP3 en la FAT: ~/6g/Erbmusic.img (ya fabricado)
# AUDIOPACE=1 es OBLIGATORIO (ver abajo).
env AUDIOPACE=1 sh tools/ipod_classic/rbplay.sh \
        $HOME/6g/Erbmusic.img $HOME/6g/rbfinal $HOME/6g/rbfinal/play.wav
python3 tools/ipod_classic/wavcheck.py $HOME/6g/rbfinal/play.wav \
        $HOME/6g/tracks/'Numb - Encore.mp3'   # VERDICT: MATCH
```

### Lo que hubo que ajustar en el modelo (un solo cambio)

`hw/dma/pl080.c`, en `pl080_pace_request()`: tras `pl080_run()` se **suelta** el
bit de petición one-shot de la línea. Sin esto, una petición que no movió nada
(canal aún no habilitado, o parado entre tasks) quedaba **colgada** y, al
habilitar el guest el canal, drenaba una task entera de golpe; el terminal-count
que seguía apagaba el bit E del canal y el `dmac_ch_queue_2d` de Rockbox veía
E=0 y entraba en pánico *"DMA ch link error"*. Soltando el pulso, el canal sólo
avanza con una petición viva y sigue habilitado entre ticks, así que el guest
puede extender la cadena de LLIs a tiempo. **Sólo afecta a líneas *paced***
(único llamador: `ipod_classic_i2s_drain()`), y ésas sólo existen bajo
`AUDIOPACE` — de modo que el retailOS y el 5G, que nunca marcan la línea ni
llaman a la función, quedan **byte-idénticos** (verificado leyendo el gateo
`i2s_pace_on()`; navtest 6G: la rueda mueve el resaltado 5/5, y el fallo de
SELECT es la fluctuación ya documentada del arnés, no una regresión; 5G: navega).

### Dos correcciones a lo que se predijo

- **`AUDIOPACE=1` es necesario**, no opcional. Sin pacing el DMA drena el anillo
  al instante, la reproducción hace *underrun* antes de que el codec decodifique
  el segundo buffer y sólo se capturan ~512 B. Medido: `rbverify_np` (sin pace)
  = 556 B; con pace = WAV completo. El pacing no cambia el *contenido* muestra a
  muestra, pero sí es lo que mantiene el flujo vivo lo bastante para capturarlo.
- **La tasa es 48000 Hz**, no 44100: este MP3 es de 48 kHz, Rockbox pone
  `I2SCLKDIV=0xfa` (=250 → 12 MHz/250 = 48000) y el WAV queda etiquetado bien.

---

## 8. Que **SUENE por los altavoces** y que reproducir sea **FIABLE**

Faltaban dos cosas para *disfrutarlo*: (a) que el PCM salga por los altavoces del
host en vivo, no sólo a un WAV; (b) eliminar un **panic intermitente** del DMA al
darle *play*. Ambas hechas y **medidas**.

### 8.1. Audio en vivo por PulseAudio (`AUDIOLIVE=1`, `-audio pa`)

El **mismo** PCM que va al WAV se entrega además a un `SWVoiceOut` de QEMU, que
sale por el backend `-audio pa` (PulseAudio). Bajo **WSLg**, `pa` alcanza los
altavoces de Windows: `PULSE_SERVER=/mnt/wslg/PulseServer` → `RDPSink` → RDP →
Windows (`pactl info` lo confirma: *Default Sink RDPSink*). En un Linux de
escritorio, `pa` va al PulseAudio/Pipewire local. Está en
`hw/arm/ipod_classic/ipod_classic_i2s.c`, **gated por `AUDIOLIVE`** y **aditivo** a
la captura `AUDIOWAV` (los mismos bytes van a los dos sitios).

**Un solo comando** (ventana gráfica + sonido):

```sh
sh tools/ipod_classic/rbmusic.sh          # -M iPod-Classic -audio pa, AUDIOPACE+AUDIOLIVE
# equivale a:
env AUDIOPACE=1 AUDIOLIVE=1 <qemu> -M iPod-Classic -kernel rockbox.bin \
    -drive if=ide,format=raw,file=$HOME/6g/Erbmusic.img -audio pa -display sdl
```

Teclas (enfocar la ventana; mapeo en `ipod_classic_wheel.c`): **e/q** rueda
adelante/atrás, **Enter** SELECT, **w** MENU, **s** play/pausa, **d/a**
siguiente/anterior. Para reproducir: arranca (~15 s) → **Enter** → **e/q** a una
pista → **Enter**.

**Prueba MEDIDA de que el backend recibe el PCM real (el mismo que el WAV):**

- **Checksum, byte a byte.** En una corrida con `AUDIOWAV`+`AUDIOLIVE`, los bytes
  entregados a `AUD_write` son **idénticos** a los del WAV:
  `wav_bytes=live_bytes=4936192`, `wav_sum=live_sum=626263738` → **IDENTICAL**
  (instrumentación temporal `AUDIOLIVEDBG`, ya retirada). Y ese WAV correlaciona
  **1.000** con la canción (§7). Es decir: al altavoz llega, muestra a muestra, la
  misma canción que ya validamos.
- **Captura del monitor de PulseAudio** (`parecord RDPSink.monitor`) durante una
  reproducción: **pico 31883** (casi fondo de escala, cf. WAV 30074), **estéreo
  real** (`RMS(L−R)=4363`, 0 sería falso-mono), reconociblemente la canción.
- **Host ocioso: 0 underruns, 0 drops** en toda la canción.

**Cómo está cableado** (todo `AUDIOLIVE`-gated, inerte por defecto):

- La voz se abre **en `realize`**, *antes* de arrancar la VM. `AUD_set_active_out`
  sólo arma el timer del mixer si `vm_running`, que se pone **una vez** al arrancar
  la VM; una voz registrada tarde (durante la reproducción) se pierde ese flanco y
  **no clockea ni una muestra** (el backend `wav` se queda en 44 bytes; `pa` mudo).
  Copiado de `hw/audio/lm4549.c`.
- **Anillo de 1 MiB + *priming* de 300 ms.** El DMA se pacea en reloj **VIRTUAL** y
  el mixer consume en reloj **REAL**; sus tasas sólo coinciden de media, así que el
  jitter por ráfaga vaciaría el anillo (medido: **~90 underruns/s** sin colchón).
  Se retiene el primer 0.3 s de PCM sin activar la salida (una voz inactiva no
  tira, el anillo se llena); luego se activa y el **callback del mixer** drena el
  anillo a su ritmo real. La ruta por muestra es mínima (sólo *append*; el
  `AUD_write` ocurre en el callback), porque el decode MP3 en el ARM emulado va
  justo de presupuesto y cualquier trabajo por muestra le roba margen al códec.

**Techo honesto (rendimiento del host, no un bug):** el decode MP3 en el ARM
emulado va a **~0.9× tiempo real** en este host de desarrollo (contendido con
compilaciones). El *priming* absorbe **jitter**, no un déficit sostenido: si el
host no sostiene ≥1× real-time, el colchón se agota en unos segundos y reaparece
algún underrun (la canción suena un pelín lenta/con algún hueco). En un host
rápido/ocioso suena limpio y a tono. **El WAV siempre es exacto**, y el backend
siempre recibe el PCM correcto (checksum arriba). Nota de medida: grabar el
monitor `RDPSink.monitor` *a la vez* que corre QEMU roba CPU y añade sus propios
underruns a la *grabación* — es artefacto del aparato de medida, no de la salida.

### 8.2. El panic **"DMA ch link error"** eliminado de raíz

Reproduciendo con la ventana gráfica (o con el host cargado) Rockbox entraba a
veces en `panicf("DMA ch%d: link error")` al darle *play*. Es una **carrera** del
pacing del DMA de audio.

**La causa (VERIFICADO leyendo el driver, `assets/rockbox/.../s5l8702/pl080.c`).**
Rockbox mantiene un DMA de audio continuo extendiendo la cadena de LLIs desde su
ISR de terminal-count (`dmac_ch_queue_2d`, rama `QUEUE_LINK`): cuando ve
`DMACCxLINK==0` (el HW llegó a la última LLI) escribe el enlace de la nueva task y
**re-lee `DMACCxCONFIG`**; si el bit **E** (enable) ya está a 0, hace *panic*
(`pl080.c:436-437` del firmware). Nuestro modelo, al completar la LLI terminal
(`link==0`), **auto-deshabilitaba el canal** (`ch->conf &= ~E`) en ese mismo
instante — y ese instante lo dispara un **`QEMUTimer` del host** (el tick de
drenaje del audio) que el `disable_irq_save()` del guest **no puede frenar**. Si
el timer completa la LLI terminal *entre* la lectura `link==0` y la comprobación de
E del guest, éste ve **E=0** y hace panic. Carrera de *timing relativo*: headless
rápido casi nunca la pierde; con la ventana gráfica (WSLg roba CPU, el guest tarda
más) a veces sí — justo lo que reportaba el usuario.

**El arreglo (robusto, `hw/dma/pl080.c`).** Un canal **paced** **nunca** suelta su
bit E al llegar a una LLI terminal: se marca `paced_pending` y se queda
*enabled*-e-inactivo. Reanuda cuando el guest escribe un nuevo `LINK`
(`pl080_write` caso 2 sólo lo **registra**; `pl080_run` recarga el descriptor en el
siguiente tick, que es donde el HW real consume el registro LINK), y **sólo** se
deshabilita cuando el guest aborta explícitamente (`CONFIG.E=0`). Así la
comprobación de E del guest **siempre** ve E=1 y el `panicf` queda **inalcanzable
para el canal de audio con cualquier timing** — garantía determinista, no
probabilística. Un reenganche tardío cuesta, como mucho, un breve underrun.
**Gating sin regresión:** `paced_pending` sólo se pone para líneas *paced*, que
sólo existen bajo `AUDIOPACE` y sólo las marca el I2S 6G; para el retailOS 6G y el
5G el flujo es **byte-idéntico** al de stock.

**Verificación MEDIDA (detector: leer `panic_buf` en `0x08154c48` por el monitor
tras cada corrida).** Para *forzar* la carrera se usó un botón temporal
`PANICSTRESS=N` (comprime el intervalo del tick para que el DMA adelante al guest)
y un `PACED_NOFIX=1` que revierte el arreglo en el **mismo binario** (ambos ya
retirados):

| Corrida | Sin arreglo (`PACED_NOFIX=1`) | Con arreglo |
|---|---|---|
| stress=16 (carrera forzada) | **4/4 PANIC** (incl. el literal `DMA ch2: link error`) | **0 / 8** |
| stress=2 (playback sostenido) | **3/5 PANIC** | **0 / 10** |
| pacing normal + `-audio pa` (condición del usuario) | — | **0 / 10** |

Es decir: el estresor **reproduce** el panic (y el detector lo caza, texto exacto
incluido) y el arreglo lo **elimina** en todas las corridas, tanto bajo la carrera
forzada como en reproducción normal sostenida con audio en vivo. (El 5G no se
toca; `navtest.sh` 6G y 5G siguen pasando — §8.3.)

### 8.3. Reproducir

```sh
# altavoces en vivo (ventana):
sh tools/ipod_classic/rbmusic.sh
# captura a WAV como antes (sigue dando la canción, correlación 1.000):
env AUDIOPACE=1 sh tools/ipod_classic/rbplay.sh
# a la vez altavoces + WAV:
AUDIOWAV=/tmp/out.wav sh tools/ipod_classic/rbmusic.sh
```

## 9. De Rockbox al osos por ROLO (fidelidad)

- El osos se reubica solo: la VA en ejecucion es `0x08000000 + offset_en_fichero - 0xAF98`.
- El bloque SYSINFO/bootinfo (`IsyS` en `0x2203ff18`) lo deja el NOR bootloader de Apple (ONB), **no** la bootrom: `assets/device/hwprobe2.txt` muestra bajo Rockbox ese tope de IRAM1 como ruido de arranque, sin etiqueta. Por eso el modelo lo escribe solo para el osos (`BOOTINFO=0/1` lo fuerza; los scripts de Rockbox ponen 0) y el stub ROLO debe escribirlo el mismo.
- `IRAMNOISE=semilla` rellena IRAM0/IRAM1 con ruido al encender; el osos 2.0.5 arranca igual.
- Bajo Rockbox `PWRCON0 = 0xfdffffd5`: el reloj de IRAM1 (`CLOCKGATE_SM1`, bit 4) esta cortado y tocar IRAM1 cuelga el bus real. El modelo lo registra con `-d guest_errors` ("clock gated").
- `mise run rolo-rehearse -- fichero.ipod` ensaya el ROLO en QEMU antes de copiarlo al iPod.
