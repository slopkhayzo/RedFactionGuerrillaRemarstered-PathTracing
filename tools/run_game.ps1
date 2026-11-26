# Queue job (atomic): an unattended run of rfg.exe in this project's private copy of the game (tools\sandbox.ps1).
# Installs Ultimate ASI Loader, rfgrt.asi and rfgrt_host.exe into the copy, runs the game, collects the run folder,
# removes everything it installed, and checks that the real save files were not touched (save writes are blocked).
# Example:
#   queue.ps1 submit -Atomic -Name 'rfgrt smoke' -Owner RedFactionGuerrillaRemarstered-RTHWAccel -TimeoutMinutes 8 `
#     -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','30'
param(
    [string]$Name = 'run',
    [switch]$AutoLoadSave,       # main menu -> Load Game -> most recent save
    [int]$ExitAfterMainMenuSeconds = 0,
    [int]$ExitAfterGameplaySeconds = 0,
    [int]$TimeoutSeconds = 300,
    [string[]]$Set = @(),        # plugin settings: 'Section.Key=Value', e.g. 'Render.ClbPattern=1;Test.ShotsAt=10,20'
    [string]$Config = 'RelWithDebInfo',
    [string]$Sandbox = (Join-Path (Split-Path -Parent $PSScriptRoot) 'game\gog'),
    # the installed game the copy is refreshed from (the Steam one for a Steam copy: -Sandbox game\steam)
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered',
    [switch]$WithPhysX,          # rfgphys too, as installed in -GameDir, for this run (its interplay with the path tracer)
    # A recording (play_session.ps1 -Record: recordings\<name>) replayed by rfgphys's harness instead of a save: the same
    # match, input and camera, screenshots (rfgphys\shot_<frame>.png) at -ShotFrames, else at the recording's marks
    [string]$Replay = '',
    [string]$ShotFrames = '',
    [string[]]$PhysXSet = @(),   # rfgphys settings for this run, 'Section.Key=Value;...' (e.g. a scripted recording)
    # rfgphys from this folder instead of -GameDir's scripts folder: one laid out the same way, e.g. the PhysX project's
    # package dist\rfgphys-<version>\scripts (rfgphys.asi, rfgphys.ini, rfgphysx\)
    [string]$PhysXFrom = ''
)
if ($Replay -or $PhysXFrom) { $WithPhysX = $true }

. "$PSScriptRoot\lib\common.ps1"
$root = Split-Path -Parent $PSScriptRoot
$asi = Join-Path $root "build-x86\$Config\rfgrt.asi"
$hostExe = Join-Path $root "build-x64\$Config\rfgrt_host.exe"
$loader = Join-Path $root 'third_party\ultimate-asi-loader\dinput8.dll'

if (Get-Process -Name rfg -ErrorAction SilentlyContinue) {
    Write-Output 'an rfg.exe is already running (someone playing, or another run): aborting'
    exit 3
}
foreach ($f in $asi, $hostExe, $loader) { if (-not (Test-Path $f)) { Write-Output "$f not found (tools\build.ps1)"; exit 2 } }
# A run never tests stale binaries (a failed build before it, say): each binary against its own sources.
$sourcesOf = @{
    $asi     = @('src\rfgrt', 'src\shared')
    $hostExe = @('host', 'src\rtcore', 'src\shared')
}
# rfgrt_present.exe (the 32-bit plugin's presenter), when the build has it
$presentExe = Join-Path (Split-Path $hostExe) 'rfgrt_present.exe'
$checked = @($asi, $hostExe)
if (Test-Path $presentExe) {
    $sourcesOf[$presentExe] = @('src\present_host', 'src\rfgrt\present\framegen.cpp', 'src\rfgrt\present\framegen.h',
                                'src\rfgrt\present\present_ipc.h', 'src\rfgrt\present\fg_motion.hlsl', 'src\rfgrt\present\fg_ui.hlsl',
                                'src\rfgrt\core\log.cpp', 'src\rfgrt\core\log.h')
    $checked += $presentExe
}
foreach ($f in $checked) {
    $newest = Get-ChildItem ($sourcesOf[$f] | ForEach-Object { Join-Path $root $_ }) -Recurse -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ((Get-Item $f).LastWriteTime -lt $newest.LastWriteTime) {
        Write-Output "$f is older than $($newest.FullName): build first (tools\build.ps1)"
        exit 2
    }
}
if ((Get-FileHash $loader -Algorithm SHA256).Hash -ne $UalHash) { Write-Output 'the loader is not Ultimate ASI Loader 9.7.4'; exit 2 }
& "$PSScriptRoot\sandbox.ps1" -GameDir $GameDir -Sandbox $Sandbox | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# What a play session (tools\play_session.ps1) left if it was ended before it could clean up: rfgphys is only in a
# test that asks for it (-WithPhysX), installed below.
foreach ($left in 'scripts\rfgphys.asi', 'scripts\rfgphys.ini', 'scripts\rfgphys.pdb', 'scripts\rfgphysx') {
    $f = Join-Path $Sandbox $left
    if (Test-Path $f) {
        Remove-Item $f -Recurse -Force
        Write-Output "removed $left left in the game copy by a play session"
    }
}

# --- install (removed again at the end, whatever happens) ---
$installed = @()
function Install-File([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Force (Split-Path $Destination) | Out-Null
    Copy-Item $Source $Destination -Force
    $script:installed += $Destination
}
$scripts = Join-Path $Sandbox 'scripts'
try {
    Install-File $loader (Join-Path $Sandbox 'dinput8.dll')
    Install-File $asi (Join-Path $scripts 'rfgrt.asi')
    Install-File ([IO.Path]::ChangeExtension($asi, '.pdb')) (Join-Path $scripts 'rfgrt.pdb')
    Install-File $hostExe (Join-Path $scripts 'rfgrt\rfgrt_host.exe')
    Install-File ([IO.Path]::ChangeExtension($hostExe, '.pdb')) (Join-Path $scripts 'rfgrt\rfgrt_host.pdb')
    # DLSS Ray Reconstruction's runtime, beside the core, when the build has it (tools\fetch_dlss.ps1)
    $dlss = Join-Path (Split-Path $hostExe) 'nvngx_dlssd.dll'
    if (Test-Path $dlss) { Install-File $dlss (Join-Path $scripts 'rfgrt\nvngx_dlssd.dll') }
    # DLSS Frame Generation: rfgrt_present.exe (the plugin's presenter, 64-bit) with Streamline's runtime DLLs beside it,
    # when the build has them (tools\fetch_streamline.ps1)
    $present = Join-Path (Split-Path $hostExe) 'rfgrt_present.exe'
    if (Test-Path $present) {
        Install-File $present (Join-Path $scripts 'rfgrt\rfgrt_present.exe')
        Install-File ([IO.Path]::ChangeExtension($present, '.pdb')) (Join-Path $scripts 'rfgrt\rfgrt_present.pdb')
        foreach ($sl in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll') {
            $f = Join-Path (Split-Path $hostExe) $sl
            if (Test-Path $f) { Install-File $f (Join-Path $scripts "rfgrt\$sl") }
        }
    }
    $physx = $false
    if ($WithPhysX) {
        $scriptsIn = if ($PhysXFrom) { $PhysXFrom } else { Join-Path $GameDir 'scripts' }
        if (Test-Path (Join-Path $scriptsIn 'rfgphys.asi')) {
            $physx = $true
            foreach ($f in Get-ChildItem $scriptsIn -File | Where-Object { $_.Name -match '^rfgphys\.(asi|pdb|ini)$' }) {
                Install-File $f.FullName (Join-Path $scripts $f.Name)
            }
            $hostDir = Join-Path $scriptsIn 'rfgphysx'
            if (Test-Path $hostDir) {
                foreach ($f in Get-ChildItem $hostDir -File -Recurse) {
                    Install-File $f.FullName (Join-Path $scripts ('rfgphysx\' + $f.FullName.Substring($hostDir.Length + 1)))
                }
            }
            Write-Output "rfgphys: from $scriptsIn (plugin of $((Get-Item (Join-Path $scriptsIn 'rfgphys.asi')).LastWriteTime))"
        } else {
            Write-Output "rfgphys is not in ${scriptsIn}: the run goes without it"
        }
    }

    $run = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Name")
    New-Item -ItemType Directory -Force $run | Out-Null
    $env:RFGRT_RUN_DIR = $run
    $env:RFGRT_TEST_EXITAFTERMAINMENUSECONDS = "$ExitAfterMainMenuSeconds"
    $env:RFGRT_TEST_EXITAFTERGAMEPLAYSECONDS = "$ExitAfterGameplaySeconds"
    $env:RFGRT_TEST_EXITAFTERSECONDS = "$([Math]::Max(30, $TimeoutSeconds - 20))"
    $env:RFGRT_TEST_AUTOLOADSAVE = $(if ($AutoLoadSave) { '1' } else { '0' })
    $env:RFGRT_TEST_BLOCKSAVEWRITES = '1'   # unattended runs never write or erase save data
    $env:RFGRT_TEST_GODMODE = '1'
    $env:RFGRT_TEST_IGNOREFOCUS = '1'       # started from a hidden job, the window is often not in front
    if ($physx) {  # rfgphys as a player has it, its logs in the run folder, never saving, its harness off
        $env:RFGPHYS_RUN_DIR = Join-Path $run 'rfgphys'
        New-Item -ItemType Directory -Force $env:RFGPHYS_RUN_DIR | Out-Null
        $env:RFGPHYS_HARNESS_MODE = 'off'
        $env:RFGPHYS_TEST_AUTOLOADSAVE = '0'
        $env:RFGPHYS_TEST_BLOCKSAVEWRITES = '1'
        if ($Replay) {
            $rec = if ($Replay -eq 'latest') {  # the newest finished recording (a job that records, then replays it)
                Get-ChildItem (Join-Path $root 'recordings') -Directory -ErrorAction SilentlyContinue |
                    Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
            } elseif (Test-Path $Replay) { (Resolve-Path $Replay).Path } else { Join-Path $root "recordings\$Replay" }
            if (-not $rec) { throw 'no finished recording in recordings' }
            $manifest = Join-Path $rec 'manifest.json'
            if (-not (Test-Path $manifest)) { throw "no recording at $rec" }
            $j = Get-Content $manifest -Raw | ConvertFrom-Json
            $frames = if ($ShotFrames) { $ShotFrames } else { (@($j.markers) | ForEach-Object { "$_" }) -join ',' }
            $env:RFGPHYS_HARNESS_MODE = 'replay'
            $env:RFGPHYS_HARNESS_RECORDING = $rec
            $env:RFGPHYS_HARNESS_SCREENSHOTFRAMES = $frames
            Write-Output "replay: $rec ($($j.frames) frames, $([Math]::Round([double]$j.game_seconds)) s; $($j.start.setup)); shots at frames $frames"
        }
        foreach ($s in ($PhysXSet | ForEach-Object { $_ -split ';' } | Where-Object { $_ })) {
            if ($s -match '^(\w+)\.(\w+)=(.*)$') {
                Set-Item -Path ("env:RFGPHYS_" + $Matches[1].ToUpper() + "_" + $Matches[2].ToUpper()) -Value $Matches[3]
                Write-Output "rfgphys setting $($Matches[1]).$($Matches[2]) = $($Matches[3])"
            }
        }
    }
    Set-PluginOverrides $Set

    $savesBefore = @(Get-SaveHashes)
    Backup-Saves $root $savesBefore
    $knownDumps = Get-CrashDumpNames $Sandbox
    Set-SteamLaunchEnv $Sandbox
    Write-Output "starting rfg.exe from $Sandbox, run folder $run"
    $p = Start-Process -FilePath (Join-Path $Sandbox 'rfg.exe') -WorkingDirectory $Sandbox -PassThru
    if ($p.WaitForExit($TimeoutSeconds * 1000)) {
        $code = $p.ExitCode
    } else {
        Write-Output "timeout after $TimeoutSeconds s: killing rfg.exe"
        Stop-Process -Id $p.Id -Force
        $code = 124
    }
    Write-Output "rfg.exe exit code: $code"
    Get-Process -Name rfgrt_host -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Sandbox*" } | ForEach-Object {
        if ($_.WaitForExit(3000)) { return }  # the job object ends it with the game; give it a moment
        Write-Output "rfgrt_host.exe still running 3 s after the game (pid $($_.Id)): ended"
        $_ | Stop-Process -Force
        $null = $_.WaitForExit(5000)
    }
    Copy-NewCrashDumps $Sandbox $knownDumps $run | Out-Null
    foreach ($gameLog in 'log_general.log', 'log_fatal.log', 'Red Faction Guerrilla Re-Mars-tered.log') {
        $f = Join-Path $Sandbox $gameLog
        if ((Test-Path $f) -and (Get-Item $f).LastWriteTime -gt $p.StartTime) { Copy-Item $f $run }
    }
} finally {
    Start-Sleep -Milliseconds 500  # the host and the game release their files
    for ($try = 0; $try -lt 10; $try++) {
        foreach ($f in $installed) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
        if (-not @($installed | Where-Object { Test-Path $_ }).Count) { break }
        Start-Sleep -Milliseconds 500
    }
    $physxRuns = Join-Path $scripts 'rfgphys_runs'
    if (Test-Path $physxRuns) {
        if ($run) { Copy-Item (Join-Path $physxRuns '*') (Join-Path $run 'rfgphys') -Recurse -Force -ErrorAction SilentlyContinue }
        Remove-Item $physxRuns -Recurse -Force -ErrorAction SilentlyContinue
    }
    foreach ($d in (Join-Path $scripts 'rfgrt'), (Join-Path $scripts 'rfgphysx'), $scripts) {
        if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d -Force }
    }
    $left = @($installed | Where-Object { Test-Path $_ })
    if ($left.Count) { Write-Output "NOT REMOVED: $($left -join ', ')" } else { Write-Output "removed the $($installed.Count) installed files" }
}

$savesAfter = @(Get-SaveHashes)
if (($savesBefore -join ';') -ne ($savesAfter -join ';')) {
    Write-Output 'SAVE FILES CHANGED during the run (backups are in runs\save_backups):'
    Write-Output ('  before: ' + ($savesBefore -join '; '))
    Write-Output ('  after:  ' + ($savesAfter -join '; '))
    exit 5
}
Write-Output "save files verified untouched ($($savesAfter.Count) files)"

$log = Join-Path $run 'rfgrt.log'
if (-not (Test-Path $log)) { Write-Output 'no rfgrt.log: the plugin did not load'; exit 2 }
Write-Output '--- rfgrt.log (tail) ---'
Get-Content $log -Tail 45
$hostLog = Join-Path $run 'rfgrt_host.log'
if (Test-Path $hostLog) { Write-Output '--- rfgrt_host.log (tail) ---'; Get-Content $hostLog -Tail 15 }
if ($code -eq 124) { exit 124 }
if (Select-String -Path $log -Pattern 'session end' -Quiet) { exit 0 }
exit 2
