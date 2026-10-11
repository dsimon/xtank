<#
.SYNOPSIS
    Native Windows 11 x64 runtime smoke test for the Xtank Phase 2 client shell.

.DESCRIPTION
    P2.9 acceptance runner.  It exercises the cross-built client archive on a
    real Windows machine (no compiler, no X server, no POSIX runtime):

      1. dependency audit - parse the PE import table of xtank-client.exe and
         fail if it imports anything outside the approved clean-machine set
         (Win32 system DLLs + the bundled SDL3.dll), or if a libgcc /
         libwinpthread / Cygwin / MSYS runtime is present next to it;
      2. window/event/timer/renderer self-test - opens a resizable high-DPI
         window, injects keyboard/mouse, resizes, renders 60 frames, verifies
         the logical/drawable coordinate transform at 100/150/200% DPI, and
         decodes/re-encodes the committed protocol golden fixtures;
      3. ENet loopback - a real UDP connect + HELLO/WELCOME codec exchange;
      4. optional live connect - with -Server HOST:PORT, connect to the Linux
         authoritative server and require the HELLO/WELCOME handshake.

    Every step is required; any failure exits non-zero.  Full output is saved
    to -LogDir for the Phase 2 owner evidence.

.PARAMETER Exe
    Path to xtank-client.exe (default: .\xtank-client.exe).

.PARAMETER Fixtures
    Directory holding the protocol golden *.hex fixtures.  Passed to the
    client with --fixtures so the fixture checks are enforced (not optional).

.PARAMETER Server
    Optional HOST:PORT of a running Linux server.  When set, the script runs
    the live HELLO/WELCOME connect check and requires CONNECT PASS.

.PARAMETER LogDir
    Directory for captured logs (default: $env:TEMP\xtank-p2.9).

.EXAMPLE
    .\windows-smoke.ps1 -Exe .\xtank-client.exe -Fixtures .\fixtures\protocol

.EXAMPLE
    .\windows-smoke.ps1 -Server 192.168.1.20:7777
#>

[CmdletBinding()]
param(
    [string] $Exe      = (Join-Path $PSScriptRoot 'xtank-client.exe'),
    [string] $Fixtures = '',
    [string] $Server   = '',
    [string] $LogDir   = (Join-Path $env:TEMP 'xtank-p2.9')
)

$ErrorActionPreference = 'Stop'
$script:Fails = 0
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Pass([string] $m) { Write-Host "PASS: $m" }
function Fail([string] $m) { Write-Host "FAIL: $m"; $script:Fails++ }
function Info([string] $m) { Write-Host "      $m" }

# ---------------------------------------------------------------------------
# Minimal PE import reader (works in Windows PowerShell 5.1 without dumpbin).
# Returns the DLL names in the import directory of a PE32/PE32+ image.
# ---------------------------------------------------------------------------
function RvaToOffset {
    param([byte[]] $Bytes, [object[]] $Sections, [uint32] $Rva)
    foreach ($sec in $Sections) {
        $vsize = [Math]::Max([uint32]$sec.VSize, [uint32]1)
        if ($Rva -ge $sec.VA -and $Rva -lt ($sec.VA + $vsize)) {
            return $sec.Raw + ($Rva - $sec.VA)
        }
    }
    throw ("RVA 0x{0:X} is not in any section" -f $Rva)
}

function Get-PeImports([string] $path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 0x40) { throw "$path is too small to be a PE" }
    $peOff = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) {
        throw "$path has no PE signature"
    }
    $coff = $peOff + 4
    $numSections      = [BitConverter]::ToUInt16($bytes, $coff + 2)
    $optSize          = [BitConverter]::ToUInt16($bytes, $coff + 16)
    $opt              = $coff + 20
    $magic            = [BitConverter]::ToUInt16($bytes, $opt)
    if ($magic -eq 0x20B) { $ddOff = $opt + 112 }
    elseif ($magic -eq 0x10B) { $ddOff = $opt + 96 }
    else { throw "$path has unknown optional-header magic 0x$('{0:X}' -f $magic)" }
    $importRva  = [BitConverter]::ToUInt32($bytes, $ddOff + 1 * 8)
    $importSize = [BitConverter]::ToUInt32($bytes, $ddOff + 1 * 8 + 4)
    if ($importRva -eq 0) { return @() }

    $secOff = $opt + $optSize
    $sections = @()
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $secOff + $i * 40
        $sections += [pscustomobject]@{
            VA   = [BitConverter]::ToUInt32($bytes, $s + 12)
            VSize= [BitConverter]::ToUInt32($bytes, $s + 8)
            Raw  = [BitConverter]::ToUInt32($bytes, $s + 20)
        }
    }
    $names = @()
    $d = RvaToOffset -Bytes $bytes -Sections $sections -Rva $importRva
    while ($true) {
        $nameRva = [BitConverter]::ToUInt32($bytes, $d)
        $firstThunk = [BitConverter]::ToUInt32($bytes, $d + 16)
        if ($nameRva -eq 0 -and $firstThunk -eq 0) { break }
        $no = RvaToOffset -Bytes $bytes -Sections $sections -Rva $nameRva
        $end = $no
        while ($bytes[$end] -ne 0) { $end++ }
        $names += [System.Text.Encoding]::ASCII.GetString($bytes, $no, $end - $no)
        $d += 20
    }
    return $names
}

# Win32 DLLs a clean Windows 11 x64 machine always provides (case-insensitive).
$ApprovedSystemDlls = @(
    'kernel32.dll', 'ntdll.dll', 'msvcrt.dll', 'api-ms-win-crt-runtime-l1-1-0.dll',
    'api-ms-win-crt-stdio-l1-1-0.dll', 'api-ms-win-crt-string-l1-1-0.dll',
    'api-ms-win-crt-heap-l1-1-0.dll', 'api-ms-win-crt-math-l1-1-0.dll',
    'api-ms-win-crt-convert-l1-1-0.dll', 'api-ms-win-crt-filesystem-l1-1-0.dll',
    'api-ms-win-crt-locale-l1-1-0.dll', 'api-ms-win-crt-time-l1-1-0.dll',
    'api-ms-win-crt-environment-l1-1-0.dll', 'api-ms-win-crt-private-l1-1-0.dll',
    'api-ms-win-core-synch-l1-2-0.dll',
    'user32.dll', 'gdi32.dll', 'advapi32.dll', 'ole32.dll', 'oleaut32.dll',
    'shell32.dll', 'shlwapi.dll', 'winmm.dll', 'ws2_32.dll', 'imm32.dll',
    'setupapi.dll', 'cfgmgr32.dll', 'version.dll', 'dwmapi.dll', 'd3d11.dll',
    'dxgi.dll', 'd3dcompiler_47.dll', 'opengl32.dll', 'bcrypt.dll', 'secur32.dll',
    'crypt32.dll', 'hid.dll', 'uxtheme.dll', 'rpcrt4.dll', 'comdlg32.dll',
    'comctl32.dll', 'userenv.dll', 'powrprof.dll', 'synchronization.dll',
    'kernelbase.dll', 'dinput8.dll', 'xinput1_4.dll', 'winhttp.dll', 'dbghelp.dll',
    'dbgcore.dll', 'avrt.dll', 'ksuser.dll', 'msacm32.dll', 'wldap32.dll'
)
# Runtime DLLs that must NOT be required on a clean machine (D2-D).
$ForbiddenDllPatterns = @('libgcc_s*', 'libwinpthread*', 'libstdc++*', 'cygwin*',
                          'msys-*', 'msys2*', 'libssp*')

Write-Host "== Xtank P2.9 Windows smoke"
Write-Host "exe:      $Exe"
Write-Host "log dir:  $LogDir"

if (-not (Test-Path -LiteralPath $Exe)) {
    Fail "client executable not found: $Exe"
    exit 1
}
$ExeDir = Split-Path -Parent (Resolve-Path -LiteralPath $Exe)

# ---------------------------------------------------------------------------
# 1. Dependency / CRT audit.
# ---------------------------------------------------------------------------
Write-Host "`n== dependency audit"
try {
    $imports = Get-PeImports $Exe
} catch {
    Fail "PE import parse: $_"
    $imports = @()
}
$lower = $imports | ForEach-Object { $_.ToLowerInvariant() }
Info ("imports: " + ($imports -join ', '))

foreach ($dll in $lower) {
    if ($dll -eq 'sdl3.dll') { continue }
    if ($ApprovedSystemDlls -notcontains $dll) {
        Fail "unexpected import not on the clean-machine allowlist: $dll"
    }
}
if ($lower -contains 'sdl3.dll') {
    Pass "imports only approved system DLLs + SDL3.dll"
} else {
    Fail "xtank-client.exe does not import SDL3.dll"
}

# SDL3.dll must be bundled next to the executable.
if (Test-Path -LiteralPath (Join-Path $ExeDir 'SDL3.dll')) {
    Pass "bundled SDL3.dll present next to the executable"
} else {
    Fail "bundled SDL3.dll is missing next to $Exe"
}

# No POSIX/gcc runtime DLLs may be required on a clean machine.
$baddlls = Get-ChildItem -LiteralPath $ExeDir -Filter '*.dll' |
    Where-Object { $n = $_.Name.ToLowerInvariant();
                   $ForbiddenDllPatterns | Where-Object { $n -like $_ } }
if ($baddlls) {
    Fail ("forbidden runtime DLL(s) present: " +
          (($baddlls | ForEach-Object Name) -join ', '))
} else {
    Pass "no libgcc/libwinpthread/Cygwin/MSYS runtime DLLs staged"
}

# ---------------------------------------------------------------------------
# 2/3. In-process self-test (window, input, DPI, codec, ENet loopback).
# ---------------------------------------------------------------------------
Write-Host "`n== self-test"
$selfLog = Join-Path $LogDir 'self-test.log'
$selfArgs = @('--self-test')
if ($Fixtures) { $selfArgs += @('--fixtures', $Fixtures) }
& $Exe @selfArgs 2>&1 | Tee-Object -FilePath $selfLog
$selfRc = $LASTEXITCODE

$selfOut = Get-Content -LiteralPath $selfLog -Raw
foreach ($tag in @('window logical=', 'resized logical=', 'codec fixtures:',
                   'WINDOWS-SMOKE PASS')) {
    if ($selfOut -match [regex]::Escape($tag)) { Pass "self-test output contains '$tag'" }
    else { Fail "self-test output missing '$tag'" }
}
$scaleMatch = [regex]::Match($selfOut, 'window logical=\d+x\d+ drawable=\d+x\d+ scale=([0-9.]+)')
if ($scaleMatch.Success) {
    $scale = [double]$scaleMatch.Groups[1].Value
    $pct = [int][Math]::Round($scale * 100)
    Info ("display scale reported: {0:P0} ({1})" -f $scale, $pct)
    if (@(100, 150, 200) -contains $pct) {
        Pass "native integration verified at $pct% DPI"
    } else {
        Info "display scale $pct% is not 100/150/200; the pure DPI transform" 
        Info "(100/150/200%) is still asserted by the self-test on every run"
    }
}
if ($selfRc -eq 0) { Pass "WINDOWS-SMOKE PASS (exit 0)" }
else { Fail "self-test exited $selfRc" }

# ---------------------------------------------------------------------------
# 4. Optional live server connect.
# ---------------------------------------------------------------------------
if ($Server) {
    Write-Host "`n== live connect to $Server"
    $connLog = Join-Path $LogDir 'connect.log'
    & $Exe --connect $Server 2>&1 | Tee-Object -FilePath $connLog
    $connRc = $LASTEXITCODE
    $connOut = Get-Content -LiteralPath $connLog -Raw
    if ($connOut -match 'CONNECT PASS' -and $connRc -eq 0) {
        Pass "HELLO/WELCOME handshake with $Server"
    } else {
        Fail "live connect to $Server failed (exit $connRc)"
    }
} else {
    Info "no -Server supplied; live handshake not exercised"
}

# ---------------------------------------------------------------------------
Write-Host ""
if ($script:Fails -eq 0) {
    Write-Host "WINDOWS-SMOKE PASS"
    exit 0
}
Write-Host "WINDOWS-SMOKE FAIL ($script:Fails check(s))"
exit 1
