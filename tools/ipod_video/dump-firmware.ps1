<#
.SYNOPSIS
    Dumps the firmware partition of a connected iPod (PortalPlayer generations).

.DESCRIPTION
    An iPod keeps its firmware in an unpartitioned area at the start of the
    disk, before the FAT32 data partition. On a Rockbox'd iPod that area holds
    both the Rockbox bootloader and Apple's original RetailOS image, neither of
    which is encrypted on PortalPlayer devices.

    This reads that area and nothing else. The disk is opened read-only, so the
    iPod is never written to; it can stay plugged in and mounted.

    The target disk is found by looking for a drive whose model reports an
    Apple iPod, rather than by hardcoding a drive number, because the numbering
    changes as devices are plugged in and out. The script refuses to run if it
    cannot find exactly one, or if the candidate looks like a system disk.

.PARAMETER Output
    Where to write the image. Defaults to assets/ipod-video-fw.bin next to the
    repository.

.EXAMPLE
    Right-click the Start menu, pick "Terminal (Administrator)", then:
    powershell -ExecutionPolicy Bypass -File C:\Desarrollo\qemu-ios\tools\ipod_video\dump-firmware.ps1
#>

[CmdletBinding()]
param(
    [string]$Output = "C:\Desarrollo\qemu-ios\assets\ipod-video-fw.bin"
)

$ErrorActionPreference = "Stop"

function Fail($msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# Raw access to \\.\PhysicalDriveN needs elevation; there is no way around it.
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "hay que ejecutarlo como administrador (boton derecho en el menu de inicio -> Terminal (Administrador))"
}

Write-Host "Buscando el iPod..." -ForegroundColor Cyan

$candidates = @(Get-CimInstance Win32_DiskDrive |
    Where-Object { $_.Model -match 'iPod' -or $_.PNPDeviceID -match 'VEN_APPLE' })

if ($candidates.Count -eq 0) { Fail "no encuentro ningun disco que sea un iPod. Esta conectado y montado?" }
if ($candidates.Count -gt 1) {
    Write-Host "Hay mas de un candidato:"
    $candidates | ForEach-Object { Write-Host ("  {0}  {1}  {2:N1} GB" -f $_.DeviceID, $_.Model, ($_.Size / 1GB)) }
    Fail "desconecta los que no toquen y vuelve a intentarlo"
}

$disk = $candidates[0]
$diskNumber = $disk.Index

# Paranoia: never touch the disk Windows booted from.
$systemDisk = (Get-Partition -DriveLetter $env:SystemDrive.TrimEnd(':') -ErrorAction SilentlyContinue).DiskNumber
if ($null -ne $systemDisk -and $systemDisk -eq $diskNumber) {
    Fail "el disco detectado ($diskNumber) es el disco del sistema. Abortando."
}

Write-Host ("  {0}" -f $disk.DeviceID)
Write-Host ("  modelo   : {0}" -f $disk.Model)
Write-Host ("  tamano   : {0:N2} GB" -f ($disk.Size / 1GB))
Write-Host ("  serie    : {0}" -f $disk.SerialNumber)

# The firmware area runs from sector 0 up to wherever the first partition
# starts, so its size is that partition's offset.
$part = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue |
        Sort-Object Offset | Select-Object -First 1
if ($null -eq $part) { Fail "el disco no tiene particiones; no puedo deducir donde acaba el firmware" }

$len = [int64]$part.Offset
if ($len -le 0)          { Fail "offset de la primera particion invalido ($len)" }
if ($len % 512 -ne 0)    { Fail "el offset ($len) no esta alineado a sector; no puedo leerlo en crudo" }
if ($len -gt 512MB)      { Fail "el offset ($len) es sospechosamente grande; abortando por si acaso" }

Write-Host ""
Write-Host ("Particion de firmware: {0} bytes ({1:N2} MB)" -f $len, ($len / 1MB)) -ForegroundColor Cyan
Write-Host ("Destino             : {0}" -f $Output)
Write-Host ""

$dir = Split-Path -Parent $Output
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$in = $null; $out = $null
try {
    # Read-only share so Windows keeping the volume mounted is not a problem.
    $in  = New-Object System.IO.FileStream("\\.\PhysicalDrive$diskNumber",
                                           [System.IO.FileMode]::Open,
                                           [System.IO.FileAccess]::Read,
                                           [System.IO.FileShare]::ReadWrite)
    $out = [System.IO.File]::Create($Output)

    $buf  = New-Object byte[] (1MB)
    $done = [int64]0
    $lastPct = -1

    while ($done -lt $len) {
        $want = [Math]::Min([int64]$buf.Length, $len - $done)
        $n = $in.Read($buf, 0, [int]$want)
        if ($n -le 0) { break }
        $out.Write($buf, 0, $n)
        $done += $n

        $pct = [int](100 * $done / $len)
        if ($pct -ne $lastPct) {
            Write-Progress -Activity "Volcando la particion de firmware" -PercentComplete $pct `
                           -Status ("{0:N1} / {1:N1} MB" -f ($done / 1MB), ($len / 1MB))
            $lastPct = $pct
        }
    }
}
finally {
    if ($out) { $out.Close() }
    if ($in)  { $in.Close() }
    Write-Progress -Activity "Volcando la particion de firmware" -Completed
}

Write-Host ""
if ($done -ne $len) {
    Write-Host ("AVISO: se leyeron {0} de {1} bytes" -f $done, $len) -ForegroundColor Yellow
} else {
    Write-Host ("OK: {0} bytes -> {1}" -f $done, $Output) -ForegroundColor Green
}

# A quick sanity check so a bad dump is obvious right away: every iPod firmware
# image starts with Apple's ASCII-art copyright banner and carries the "]ih["
# directory magic at 0x100.
$head = New-Object byte[] 260
$fs = [System.IO.File]::OpenRead($Output)
try { [void]$fs.Read($head, 0, $head.Length) } finally { $fs.Close() }

$banner = [System.Text.Encoding]::ASCII.GetString($head, 0, 8)
$magic  = [System.Text.Encoding]::ASCII.GetString($head, 256, 4)

Write-Host ""
Write-Host ("Primeros bytes : '{0}'" -f $banner)
Write-Host ("Magic en 0x100 : '{0}'" -f $magic)
if ($magic -eq ']ih[') {
    Write-Host "Parece una particion de firmware de iPod valida." -ForegroundColor Green
} else {
    Write-Host "No veo el magic ']ih[' en 0x100. El volcado puede no ser lo que esperamos." -ForegroundColor Yellow
}
