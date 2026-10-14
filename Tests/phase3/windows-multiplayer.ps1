<#
.SYNOPSIS
    Xtank Phase 3 two-PC Windows 11 acceptance runner (P3.16 owner exit).

.DESCRIPTION
    This script is the documented owner procedure for the two-PC exit that
    CANNOT be performed on the Linux build host:

      * Verify mode  - on a clean Windows 11 x64 machine, audit the unpacked
        client bundle (PE import allowlist, bundled SDL3.dll, VERSION /
        PINNED-RUNTIMES / CONTENT-MANIFEST / MANIFEST, assets, tools), run the
        in-process self-test, and optionally check the live HELLO/WELCOME
        handshake against a running Linux server.  A Verify PASS plus the
        ten-match Accept run plus both sign-offs are the owner exit.
      * Accept mode  - drive ten two-human matches across both mazes and 0/1/2
        drones (including four vehicles), recording each match's outcome and
        the operator's confirmations (natural 1,000-point winner, correct
        results, controls/HUD/map/radar, death/respawn, same-process
        restart/rejoin, administrator transfer).  Refuses to write a PASS
        unless all ten are recorded and every confirmation is given.
      * Solo mode    - one solo-vs-drone match for the owner solo acceptance.

    The authoritative server runs on Linux (Ubuntu 24.04, packaged
    xtank-server).  Each Windows PC unpacks the client archive (paths with
    spaces are fine), runs Verify, then joins the server by IP/port.  Nothing
    is installed and no compiler, X server or POSIX runtime is required.

.PARAMETER Exe
    Path to xtank-client.exe (default: .\xtank-client.exe next to this script
    or ..\bin\xtank-client.exe inside the bundle).

.PARAMETER Bundle
    Bundle root (default: parent of the tools/ directory holding this script).

.PARAMETER Fixtures
    Protocol golden *.hex fixture directory (default: <Bundle>\fixtures\protocol).

.PARAMETER Server
    HOST:PORT of the running Linux authoritative server.

.PARAMETER PlayerName
    Lobby name for this PC's player.

.PARAMETER Matches
    Number of two-human matches required in Accept mode (default 10).

.PARAMETER Mode
    Verify | Accept | Solo (default Verify).

.PARAMETER LogDir
    Directory for logs and the acceptance report (default: $env:TEMP\xtank-p3.16).

.EXAMPLE
    .\windows-multiplayer.ps1 -Mode Verify

.EXAMPLE
    .\windows-multiplayer.ps1 -Mode Verify -Server 192.168.1.20:7777

.EXAMPLE
    .\windows-multiplayer.ps1 -Mode Solo -Server 192.168.1.20:7777 -PlayerName Alice

.EXAMPLE
    .\windows-multiplayer.ps1 -Mode Accept -Server 192.168.1.20:7777 `
        -PlayerName Alice -Matches 10
#>

[CmdletBinding()]
param(
    [string] $Exe        = '',
    [string] $Bundle     = '',
    [string] $Fixtures   = '',
    [string] $Server     = '',
    [string] $PlayerName = $env:USERNAME,
    [int]    $Matches    = 10,
    [ValidateSet('Verify','Accept','Solo')][string] $Mode = 'Verify',
    [string] $LogDir     = (Join-Path $env:TEMP 'xtank-p3.16')
)

$ErrorActionPreference = 'Stop'
$script:Fails = 0
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$ReportPath = Join-Path $LogDir 'xtank-phase3-acceptance.json'

function Pass([string] $m) { Write-Host "PASS: $m" }
function Fail([string] $m) { Write-Host "FAIL: $m"; $script:Fails++ }
function Info([string] $m) { Write-Host "      $m" }
function Confirm([string] $q) {
    $a = Read-Host "$q [y/N]"
    return ($a -match '^(y|yes)$')
}

# ---------------------------------------------------------------------------
# Resolve the bundle / exe / fixtures.
# ---------------------------------------------------------------------------
if (-not $Bundle) {
    if ($PSScriptRoot -and (Test-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'bin'))) {
        $Bundle = Split-Path -Parent $PSScriptRoot
    } else {
        $Bundle = (Get-Location).Path
    }
}
if (-not $Exe) {
    foreach ($cand in @(
        (Join-Path $Bundle 'bin\xtank-client.exe'),
        (Join-Path $Bundle 'xtank-client.exe'),
        (Join-Path $PSScriptRoot 'xtank-client.exe'))) {
        if (Test-Path -LiteralPath $cand) { $Exe = $cand; break }
    }
}
if (-not $Fixtures) { $Fixtures = Join-Path $Bundle 'fixtures\protocol' }
$Assets = Join-Path $Bundle 'assets\xtank-assets.bin'

Write-Host "== Xtank Phase 3 two-PC acceptance (mode $Mode)"
Write-Host "bundle:   $Bundle"
Write-Host "exe:      $Exe"
Write-Host "assets:   $Assets"
Write-Host "fixtures: $Fixtures"
Write-Host "log dir:  $LogDir"
Write-Host ""

# ---------------------------------------------------------------------------
# Minimal PE import reader (Windows PowerShell 5.1, no dumpbin needed).
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
    $peOff = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { throw "no PE signature" }
    $coff = $peOff + 4
    $numSections = [BitConverter]::ToUInt16($bytes, $coff + 2)
    $optSize     = [BitConverter]::ToUInt16($bytes, $coff + 16)
    $opt         = $coff + 20
    $magic       = [BitConverter]::ToUInt16($bytes, $opt)
    if ($magic -eq 0x20B) { $ddOff = $opt + 112 }
    elseif ($magic -eq 0x10B) { $ddOff = $opt + 96 }
    else { throw "unknown optional-header magic" }
    $importRva = [BitConverter]::ToUInt32($bytes, $ddOff + 1 * 8)
    if ($importRva -eq 0) { return @() }
    $secOff = $opt + $optSize
    $sections = @()
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $secOff + $i * 40
        $sections += [pscustomobject]@{
            VA    = [BitConverter]::ToUInt32($bytes, $s + 12)
            VSize = [BitConverter]::ToUInt32($bytes, $s + 8)
            Raw   = [BitConverter]::ToUInt32($bytes, $s + 20)
        }
    }
    $names = @()
    $d = RvaToOffset -Bytes $bytes -Sections $sections -Rva $importRva
    while ($true) {
        $nameRva    = [BitConverter]::ToUInt32($bytes, $d + 12)
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

$Approved = @(
    'kernel32.dll','ntdll.dll','msvcrt.dll','winmm.dll','ws2_32.dll','sdl3.dll',
    'user32.dll','gdi32.dll','advapi32.dll','ole32.dll','oleaut32.dll',
    'shell32.dll','shlwapi.dll','imm32.dll','setupapi.dll','cfgmgr32.dll',
    'version.dll','dwmapi.dll','d3d11.dll','dxgi.dll','d3dcompiler_47.dll',
    'opengl32.dll','bcrypt.dll','secur32.dll','crypt32.dll','hid.dll',
    'uxtheme.dll','rpcrt4.dll','comdlg32.dll','comctl32.dll','userenv.dll',
    'powrprof.dll','synchronization.dll','kernelbase.dll','dinput8.dll',
    'xinput1_4.dll','winhttp.dll','dbghelp.dll','dbgcore.dll','avrt.dll',
    'ksuser.dll','msacm32.dll','wldap32.dll'
)
$Forbidden = @('libgcc_s*','libwinpthread*','libstdc++*','cygwin*','msys-*','msys2*','libssp*')

function Test-Bundle {
    if (-not (Test-Path -LiteralPath $Exe)) { Fail "client executable not found: $Exe"; return $false }
    $exeDir = Split-Path -Parent (Resolve-Path -LiteralPath $Exe)

    # Dependency audit.
    try { $imports = Get-PeImports $Exe } catch { Fail "PE import parse: $_"; $imports = @() }
    $lower = $imports | ForEach-Object { $_.ToLowerInvariant() }
    Info ("imports: " + ($imports -join ', '))
    foreach ($dll in $lower) {
        if ($dll -like 'api-ms-win-*') { continue }
        if ($Approved -notcontains $dll) { Fail "unexpected import: $dll" }
    }
    if ($lower -contains 'sdl3.dll') { Pass "imports only approved system DLLs + bundled SDL3.dll" }
    else { Fail "xtank-client.exe does not import SDL3.dll" }
    if (Test-Path -LiteralPath (Join-Path $exeDir 'SDL3.dll')) { Pass "bundled SDL3.dll present" }
    else { Fail "bundled SDL3.dll missing" }
    $baddlls = Get-ChildItem -LiteralPath $exeDir -Filter '*.dll' -ErrorAction SilentlyContinue |
        Where-Object { $n = $_.Name.ToLowerInvariant();
                       $Forbidden | Where-Object { $n -like $_ } }
    if ($baddlls) { Fail ("forbidden runtime DLL(s): " + (($baddlls | ForEach-Object Name) -join ', ')) }
    else { Pass "no libgcc/libwinpthread/Cygwin/MSYS runtime DLLs staged" }

    # Release metadata / content.
    foreach ($f in @('VERSION','PINNED-RUNTIMES.txt','CONTENT-MANIFEST.sha256',
                     'MANIFEST.sha256','THIRD-PARTY-NOTICES.txt')) {
        if (Test-Path -LiteralPath (Join-Path $Bundle $f)) {
            Pass "release metadata present: $f"
        } else { Fail "release metadata missing: $f" }
    }
    if (Test-Path -LiteralPath $Assets) { Pass "asset bundle present: $Assets" }
    else { Fail "asset bundle missing: $Assets" }
    if (Test-Path -LiteralPath $Fixtures) { Pass "protocol fixtures present" }
    else { Fail "protocol fixtures missing: $Fixtures" }

    # In-process self-test.
    Write-Host "`n== self-test"
    $selfLog = Join-Path $LogDir 'self-test.log'
    $selfArgs = @('--self-test','--fixtures',$Fixtures)
    & $Exe @selfArgs 2>&1 | Tee-Object -FilePath $selfLog | Out-Null
    $selfRc = $LASTEXITCODE
    $selfOut = Get-Content -LiteralPath $selfLog -Raw
    foreach ($tag in @('WINDOWS-SMOKE PASS','codec fixtures:')) {
        if ($selfOut -match [regex]::Escape($tag)) { Pass "self-test contains '$tag'" }
        else { Fail "self-test missing '$tag'" }
    }
    if ($selfRc -eq 0) { Pass "WINDOWS-SMOKE PASS (exit 0)" } else { Fail "self-test exit $selfRc" }

    # Optional live handshake.
    if ($Server) {
        Write-Host "`n== live connect to $Server"
        $connLog = Join-Path $LogDir 'connect.log'
        & $Exe --connect $Server 2>&1 | Tee-Object -FilePath $connLog | Out-Null
        $crc = $LASTEXITCODE
        if ((Get-Content -LiteralPath $connLog -Raw) -match 'CONNECT PASS' -and $crc -eq 0) {
            Pass "HELLO/WELCOME handshake with $Server"
        } else { Fail "live connect to $Server failed (exit $crc)" }
    } else {
        Info "no -Server supplied; live handshake not exercised (run with -Server for exit)"
    }
    return ($script:Fails -eq 0)
}

function Play-Match([string] $label) {
    Write-Host "`n$label"
    $playLog = Join-Path $LogDir ("play-" + ($label -replace '[^A-Za-z0-9_.-]','_') + ".log")
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Exe --play $Server --assets $Assets --name $PlayerName 2>&1 |
        Tee-Object -FilePath $playLog | Out-Null
    $rc = $LASTEXITCODE
    $sw.Stop()
    Info ("exit=$rc elapsed={0:N1}s log=$playLog" -f $sw.Elapsed.TotalSeconds)
    return [pscustomobject]@{ rc = $rc; seconds = [Math]::Round($sw.Elapsed.TotalSeconds,1); log = $playLog }
}

# ---------------------------------------------------------------------------
# Verify mode.
# ---------------------------------------------------------------------------
Write-Host ""
if ($Mode -eq 'Verify') {
    $ok = Test-Bundle
    Write-Host ""
    if ($ok) { Write-Host "WINDOWS-MULTIPLAYER VERIFY PASS"; exit 0 }
    Write-Host "WINDOWS-MULTIPLAYER VERIFY FAIL ($script:Fails)"; exit 1
}

# ---------------------------------------------------------------------------
# Solo mode (owner solo-vs-drone acceptance).
# ---------------------------------------------------------------------------
if ($Mode -eq 'Solo') {
    if (-not $Server) { Fail "-Mode Solo requires -Server HOST:PORT"; exit 2 }
    [void](Test-Bundle)
    $m = Play-Match "solo-vs-drone (server: 1 human + 1 drone)"
    $fields = [ordered]@{
        mode = 'solo'; server = $Server; player = $PlayerName
        match = $m; date = (Get-Date).ToUniversalTime().ToString('o')
    }
    foreach ($q in @('Solo match completed against a drone',
                     'Controls felt correct (drive/aim/fire/weapon)',
                     'HUD/map/radar correct and death/respawn correct')) {
        $fields[$q] = Confirm $q
    }
    $fields | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $LogDir 'xtank-phase3-solo.json')
    if (($fields.Values | Where-Object { $_ -eq $false }).Count -eq 0 -and $script:Fails -eq 0) {
        Write-Host "WINDOWS-MULTIPLAYER SOLO ACCEPTANCE PASS"; exit 0
    }
    Write-Host "WINDOWS-MULTIPLAYER SOLO ACCEPTANCE FAIL"; exit 1
}

# ---------------------------------------------------------------------------
# Accept mode (ten two-human matches).
# ---------------------------------------------------------------------------
if ($Mode -eq 'Accept') {
    if (-not $Server) { Fail "-Mode Accept requires -Server HOST:PORT"; exit 2 }
    [void](Test-Bundle)

    $mazes = @('Arena','combat1')
    $rows = @()
    for ($i = 1; $i -le $Matches; $i++) {
        $maze = $mazes[($i - 1) % $mazes.Count]
        $drones = ($i - 1) % 3
        $fourVeh = ($drones -eq 2)
        Write-Host ("`n===== two-human match {0}/{1}: maze={2} drones={3}{4} =====" -f `
            $i, $Matches, $maze, $drones, $(if ($fourVeh) {' (four vehicles)'} else {''}))
        Info "Admin (either PC): select maze=$maze, drones=$drones, both players ready, start."
        $m = Play-Match "match $i ($maze, $drones drones)"
        $row = [ordered]@{
            index = $i; maze = $maze; drones = $drones; four_vehicles = $fourVeh
            exit_code = $m.rc; seconds = $m.seconds; log = $m.log
            natural_1000_winner = (Confirm "Did a player reach a natural 1,000-point win?")
            correct_results = (Confirm "Were results correct (names/scores/winner)?")
            controls_ok = (Confirm "Controls correct (drive/aim/fire/weapon)?")
            hud_map_radar_ok = (Confirm "HUD/map/radar correct?")
            death_respawn_ok = (Confirm "Death/respawn correct?")
            restart_rejoin_ok = (Confirm "Same-process restart/results->lobby/rejoin correct?")
        }
        $rows += $row
    }
    $adminTransfer = Confirm "Was administrator transfer to the remaining human correct when the admin departed?"
    $pathsDpiFocus = Confirm "Smoke-tested paths with spaces, DPI scaling and focus/alt-tab?"
    $installFeel = Confirm "Owner install/feel sign-off: both PCs installed and felt correct?"

    $report = [ordered]@{
        task = 'P3.16'; kind = 'two-PC-owner-exit'; mode = 'accept'
        server = $Server; player = $PlayerName; matches_required = $Matches
        matches = $rows; admin_transfer_ok = $adminTransfer
        paths_dpi_focus_ok = $pathsDpiFocus; install_feel_signoff = $installFeel
        date = (Get-Date).ToUniversalTime().ToString('o')
        operator = $env:USERNAME; host = $env:COMPUTERNAME
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath
    Info "report: $ReportPath"

    $badRows = $rows | Where-Object {
        (-not $_.natural_1000_winner) -or (-not $_.correct_results) -or
        (-not $_.controls_ok) -or (-not $_.hud_map_radar_ok) -or
        (-not $_.death_respawn_ok) -or (-not $_.restart_rejoin_ok)
    }
    if ($rows.Count -eq $Matches -and -not $badRows -and $adminTransfer `
            -and $pathsDpiFocus -and $installFeel -and $script:Fails -eq 0) {
        Write-Host ""
        Write-Host "WINDOWS-MULTIPLAYER TWO-PC ACCEPTANCE PASS ($Matches matches)"
        exit 0
    }
    Write-Host ""
    Write-Host "WINDOWS-MULTIPLAYER TWO-PC ACCEPTANCE FAIL/INCOMPLETE"
    exit 1
}
