# Queue job for a play session with the path tracer, you at the keyboard (the queue app's profile "Play with the path
# tracer"). The latest rfgrt goes into this project's own copy of the game (tools\sandbox.ps1: game\gog, or game\steam
# for the Steam copy), never into the installed game: the release package from dist\ (default) or the development
# build, with its release settings (tools\release\rfgrt.ini). Optionally rfgphys too, as installed in the game folder
# (its plugin, host and settings, copied for the session: its PhysX debris), and Sledge, the mod manager, as installed
# there (its loader dinput8.dll, sledge.dll, its settings and mods, copied for the session: it lists the custom Wrecking
# Crew maps, whose map files are data the copy links anyway; the ASI loader then goes in as winmm.dll, as in the
# installed game). The game starts, the job waits until you quit, then everything installed is removed again; the logs
# go to runs\<time>-play-<name>. Saves are your own: written normally (backed up into runs\save_backups first), or
# blocked with -BlockSaveWrites. With -Record (needs -WithPhysX) rfgphys's harness records every Wrecking Crew match and
# save load into this project's recordings\ (its research/docs/04-test-harness.md): F11 marks a view for the agent to
# look at, F10 ends a recording early; the path tracer's probe (F10) is off and its on/off switch moves to F9 (it only
# changes the picture: the recording stays exact).
# tools\run_game.ps1 -Replay replays a recording exactly, with screenshots at its marks.
param(
    [string]$Name = 'play',
    [int]$Minutes = 60,                 # the plugin ends the game a minute before this, the queue 5 minutes after
    [ValidateSet('package', 'build')]
    [string]$Source = 'package',        # package: the newest dist\rfgrt-*-with-asi-loader.zip; build: build-x86/x64
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered',
    [switch]$WithPhysX,                 # rfgphys as installed in $GameDir (plugin, host and its settings)
    [switch]$WithSledge,                # Sledge as installed in $GameDir (its loader, sledge.dll, settings and mods)
    [switch]$BlockSaveWrites,           # the game writes no saves in this session
    [switch]$Record,                    # rfgphys's harness records each match / save load (F11: a mark; needs -WithPhysX)
    [string]$Sky = '',                  # GI.Sky for this session (empty: the release settings')
    [string]$GiDebug = '',              # GI.Debug for this session (empty: off)
    [string[]]$Set = @()                # more plugin overrides: 'Section.Key=Value;Section.Key=Value'
)

. "$PSScriptRoot\lib\common.ps1"
$root = Split-Path -Parent $PSScriptRoot

if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is already running: close it first'; exit 3 }
if (-not (Test-Path (Join-Path $GameDir 'rfg.exe'))) { Write-Output "rfg.exe not found in $GameDir"; exit 2 }
$steam = Test-Path (Join-Path $GameDir 'steam_api.dll')
$Sandbox = Join-Path $root $(if ($steam) { 'game\steam' } else { 'game\gog' })
$run = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-play-$Name")
New-Item -ItemType Directory -Force $run | Out-Null

# --- what goes into the game copy: destination (relative to it) -> source file ---
$files = [ordered]@{}
if ($Source -eq 'package') {
    $zips = @(Get-ChildItem (Join-Path $root 'dist') -Filter 'rfgrt-*-with-asi-loader.zip' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^rfgrt-([0-9.]+)-with-asi-loader\.zip$' } |
        Sort-Object { [version]($_.Name -replace '^rfgrt-([0-9.]+)-with-asi-loader\.zip$', '$1') })
    if (-not $zips.Count) { Write-Output 'no package in dist\ (tools\package.py)'; exit 2 }
    $zip = $zips[-1]
    $pkg = Join-Path $run 'package'
    Expand-Archive -Path $zip.FullName -DestinationPath $pkg -Force
    foreach ($rel in 'dinput8.dll', 'scripts\rfgrt.asi', 'scripts\rfgrt.ini', 'scripts\rfgrt\rfgrt_host.exe', 'scripts\rfgrt\nvngx_dlssd.dll',
                     'scripts\rfgrt\rfgrt_present.exe', 'scripts\rfgrt\sl.interposer.dll', 'scripts\rfgrt\sl.common.dll',
                     'scripts\rfgrt\sl.dlss_g.dll', 'scripts\rfgrt\sl.reflex.dll', 'scripts\rfgrt\sl.pcl.dll', 'scripts\rfgrt\nvngx_dlssg.dll') {
        # (the DLSS runtime: packages from 0.1.6; frame generation's presenter and Streamline: later ones)
        if (Test-Path (Join-Path $pkg $rel)) { $files[$rel] = Join-Path $pkg $rel }
    }
    Write-Output "path tracer: the package $($zip.Name)"
} else {
    $asi = Join-Path $root 'build-x86\RelWithDebInfo\rfgrt.asi'
    $hostExe = Join-Path $root 'build-x64\RelWithDebInfo\rfgrt_host.exe'
    foreach ($f in $asi, $hostExe) { if (-not (Test-Path $f)) { Write-Output "$f not found (tools\build.ps1)"; exit 2 } }
    $sourcesOf = @{ $asi = @('src\rfgrt', 'src\shared'); $hostExe = @('host', 'src\rtcore', 'src\shared') }
    foreach ($f in $asi, $hostExe) {
        $newest = Get-ChildItem ($sourcesOf[$f] | ForEach-Object { Join-Path $root $_ }) -Recurse -File |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ((Get-Item $f).LastWriteTime -lt $newest.LastWriteTime) {
            Write-Output "$f is older than $($newest.FullName): the development build is being changed, use the package"
            exit 2
        }
    }
    $files['dinput8.dll'] = Join-Path $root 'third_party\ultimate-asi-loader\dinput8.dll'
    $files['scripts\rfgrt.asi'] = $asi
    $files['scripts\rfgrt.pdb'] = [IO.Path]::ChangeExtension($asi, '.pdb')
    $files['scripts\rfgrt.ini'] = Join-Path $root 'tools\release\rfgrt.ini'
    $files['scripts\rfgrt\rfgrt_host.exe'] = $hostExe
    $files['scripts\rfgrt\rfgrt_host.pdb'] = [IO.Path]::ChangeExtension($hostExe, '.pdb')
    $dlss = Join-Path (Split-Path $hostExe) 'nvngx_dlssd.dll'
    if (Test-Path $dlss) { $files['scripts\rfgrt\nvngx_dlssd.dll'] = $dlss }
    # DLSS Frame Generation: the plugin's presenter (64-bit) and Streamline's runtime, when the build has them
    $present = Join-Path (Split-Path $hostExe) 'rfgrt_present.exe'
    if (Test-Path $present) {
        $files['scripts\rfgrt\rfgrt_present.exe'] = $present
        foreach ($sl in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll') {
            $f = Join-Path (Split-Path $hostExe) $sl
            if (Test-Path $f) { $files["scripts\rfgrt\$sl"] = $f }
        }
    }
    Write-Output "path tracer: the development build of $((Get-Item $asi).LastWriteTime)"
}
if ((Get-FileHash $files['dinput8.dll'] -Algorithm SHA256).Hash -ne $UalHash) { Write-Output 'the loader is not Ultimate ASI Loader 9.7.4'; exit 2 }

# Sledge: its own loader is the game folder's dinput8.dll (it loads sledge\sledge.dll and the mods sledge.toml enables);
# Ultimate ASI Loader then goes in as winmm.dll, as the installed game has it. Copies only (Sledge rewrites its settings
# and writes its log in the copy), removed after the session with the copy's sledge folder.
$sledge = $false
if ($WithSledge) {
    $sledgeIn = Join-Path $GameDir 'sledge'
    $proxy = Join-Path $GameDir 'dinput8.dll'
    if ((Test-Path (Join-Path $sledgeIn 'sledge.dll')) -and (Test-Path $proxy) -and
        (Get-FileHash $proxy -Algorithm SHA256).Hash -ne $UalHash) {
        $sledge = $true
        $files['winmm.dll'] = $files['dinput8.dll']
        $files['dinput8.dll'] = $proxy
        # (all but its logs: its loader finds the folder by launcher.exe and sledge.dll in it)
        foreach ($f in Get-ChildItem $sledgeIn -File -Recurse | Where-Object { $_.Extension -ne '.log' }) {
            $files['sledge\' + $f.FullName.Substring($sledgeIn.Length + 1)] = $f.FullName
        }
        $mods = @(Get-ChildItem (Join-Path $sledgeIn 'mods') -Directory -ErrorAction SilentlyContinue).Count
        Write-Output "Sledge: as installed in $GameDir (its loader as dinput8.dll, $mods mods; the ASI loader as winmm.dll)"
    } else {
        Write-Output "Sledge is not installed in ${GameDir}: playing without it"
    }
}

$physx = $false
if ($WithPhysX) {
    $scriptsIn = Join-Path $GameDir 'scripts'
    if (Test-Path (Join-Path $scriptsIn 'rfgphys.asi')) {
        $physx = $true
        foreach ($f in Get-ChildItem $scriptsIn -File | Where-Object { $_.Name -match '^rfgphys\.(asi|pdb|ini)$' }) {
            $files["scripts\$($f.Name)"] = $f.FullName
        }
        $hostDir = Join-Path $scriptsIn 'rfgphysx'
        if (Test-Path $hostDir) {
            foreach ($f in Get-ChildItem $hostDir -File -Recurse) {
                $files['scripts\rfgphysx\' + $f.FullName.Substring($hostDir.Length + 1)] = $f.FullName
            }
        }
        $env:RFGPHYS_RUN_DIR = Join-Path $run 'rfgphys'
        $env:RFGPHYS_HARNESS_MODE = $(if ($Record) { 'record' } else { 'off' })
        $env:RFGPHYS_HARNESS_RECORDINGSDIR = Join-Path $root 'recordings'
        $env:RFGPHYS_TEST_AUTOLOADSAVE = '0'
        $env:RFGPHYS_TEST_BLOCKSAVEWRITES = $(if ($BlockSaveWrites) { '1' } else { '0' })
        New-Item -ItemType Directory -Force $env:RFGPHYS_RUN_DIR | Out-Null
        $marker = Join-Path $scriptsIn 'rfgphys_installed_package.txt'
        $what = if (Test-Path $marker) { (Get-Content $marker | Select-Object -First 1) } else { 'a development deployment' }
        Write-Output "rfgphys: as installed in $GameDir ($what; plugin of $((Get-Item (Join-Path $scriptsIn 'rfgphys.asi')).LastWriteTime))"
    } else {
        Write-Output "rfgphys is not installed in ${GameDir}: playing without it"
    }
}

# --- the game copy: refreshed from the installed game, then the files above put in ---
& "$PSScriptRoot\sandbox.ps1" -GameDir $GameDir -Sandbox $Sandbox | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($rel in 'dinput8.dll', 'winmm.dll', 'sledge', 'scripts\rfgrt.asi', 'scripts\rfgphys.asi') {
    if (Test-Path (Join-Path $Sandbox $rel)) { Write-Output "the game copy already has $rel (a run that did not clean up): replaced, and removed after" }
}

$installed = @()
$code = 0
try {
    foreach ($rel in $files.Keys) {
        $dst = Join-Path $Sandbox $rel
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $files[$rel] $dst -Force
        $installed += $dst
    }
    Write-Output "installed $($installed.Count) files into $Sandbox"

    # The session's settings: the release settings (the installed ini), the logs into the run folder, the game ended a
    # minute before the queue's limit, saves as chosen, and the options picked in the app.
    $env:RFGRT_RUN_DIR = $run
    $env:RFGRT_TEST_EXITAFTERSECONDS = "$([Math]::Max(60, $Minutes * 60 - 60))"
    $env:RFGRT_TEST_BLOCKSAVEWRITES = $(if ($BlockSaveWrites) { '1' } else { '0' })
    $env:RFGRT_TEST_AUTOLOADSAVE = '0'
    $overrides = @()
    if ($Record -and $physx) {  # F10 and F11 are the recorder's: the path tracer's probe off, its on/off switch on F9
        $overrides += 'Test.ProbeKey=0', 'Test.VanillaKey=120'
        New-Item -ItemType Directory -Force (Join-Path $root 'recordings') | Out-Null
        Write-Output "recording: every Wrecking Crew match and save load into $(Join-Path $root 'recordings'); F11 marks a view, F10 ends a recording, F9 switches the path tracing off and on"
    } elseif ($Record) {
        Write-Output 'recording needs rfgphys (Also load rfgphys): this session is not recorded'
    }
    $sessionStart = Get-Date
    if ($Sky -ne '') { $overrides += "GI.Sky=$Sky" }
    if ($GiDebug -ne '') { $overrides += "GI.Debug=$GiDebug" }
    Set-PluginOverrides ($overrides + $Set)

    $savesBefore = @(Get-SaveHashes)
    Backup-Saves $root $savesBefore
    $knownDumps = Get-CrashDumpNames $Sandbox
    Set-SteamLaunchEnv $Sandbox
    Write-Output ("starting rfg.exe for your session (up to $Minutes min) from $Sandbox, " +
        $(if ($physx) { 'with rfgphys' } else { 'without rfgphys' }) + ', ' +
        $(if ($sledge) { 'with Sledge' } else { 'without Sledge' }) + "; logs in $run")
    $p = Start-Process -FilePath (Join-Path $Sandbox 'rfg.exe') -WorkingDirectory $Sandbox -PassThru
    # Sledge's own failure at start ("Failed to create hook: post_event", in streaks; the PhysX project's runs see it too):
    # its box holds the game before the first frame, and the game crashes once it is closed. Started again instead, up to
    # three times, before anyone has to click it.
    for ($try = 1; $sledge -and $try -le 3; $try++) {
        $box = $false
        for ($s = 0; $s -lt 60 -and -not $p.HasExited; $s++) {  # its first 30 s
            Start-Sleep -Milliseconds 500
            $p.Refresh()
            if ($p.MainWindowTitle -eq 'Sledge') { $box = $true; break }
        }
        if (-not $box) { break }
        Write-Output "Sledge could not start (its own failure, which comes in streaks): the game is started again ($try of 3)"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $null = $p.WaitForExit(10000)
        foreach ($helper in 'rfgrt_host', 'rfgphysx_host') {
            Get-Process -Name $helper -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Sandbox*" } |
                ForEach-Object { if (-not $_.WaitForExit(5000)) { $_ | Stop-Process -Force } }
        }
        $p = Start-Process -FilePath (Join-Path $Sandbox 'rfg.exe') -WorkingDirectory $Sandbox -PassThru
    }
    $p.WaitForExit()
    $code = $p.ExitCode
    Write-Output "rfg.exe exited with code $code"
    foreach ($helper in 'rfgrt_host', 'rfgphysx_host') {
        Get-Process -Name $helper -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Sandbox*" } | ForEach-Object {
            if ($_.WaitForExit(5000)) { return }
            Write-Output "$helper.exe still running 5 s after the game (pid $($_.Id)): ended"
            $_ | Stop-Process -Force
            $null = $_.WaitForExit(5000)
        }
    }
    Copy-NewCrashDumps $Sandbox $knownDumps $run | Out-Null
    foreach ($gameLog in 'log_general.log', 'log_fatal.log', 'Red Faction Guerrilla Re-Mars-tered.log', 'sledge\sledge.log') {
        $f = Join-Path $Sandbox $gameLog
        if ((Test-Path $f) -and (Get-Item $f).LastWriteTime -gt $p.StartTime) { Copy-Item $f $run }
    }
} finally {
    Start-Sleep -Milliseconds 500  # the helpers and the game release their files
    for ($try = 0; $try -lt 10; $try++) {
        foreach ($f in $installed) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
        if (-not @($installed | Where-Object { Test-Path $_ }).Count) { break }
        Start-Sleep -Milliseconds 500
    }
    # rfgphys's own session logs, if it kept them in the copy: into the run folder
    $physxRuns = Join-Path $Sandbox 'scripts\rfgphys_runs'
    if (Test-Path $physxRuns) {
        New-Item -ItemType Directory -Force (Join-Path $run 'rfgphys') | Out-Null
        Copy-Item (Join-Path $physxRuns '*') (Join-Path $run 'rfgphys') -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $physxRuns -Recurse -Force -ErrorAction SilentlyContinue
    }
    foreach ($d in 'scripts\rfgrt', 'scripts\rfgphysx', 'scripts') {
        $p2 = Join-Path $Sandbox $d
        if ((Test-Path $p2) -and -not (Get-ChildItem $p2 -Force)) { Remove-Item $p2 -Force }
    }
    # Sledge's folder in the copy: only the copies put there above and what Sledge wrote beside them (its log, its
    # rewritten settings); never a link to the installed game's
    $sledgeCopy = Join-Path $Sandbox 'sledge'
    if ($sledge -and (Test-Path $sledgeCopy) -and -not (Get-Item $sledgeCopy).LinkType) {
        Remove-Item $sledgeCopy -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $sledgeCopy) { Write-Output "NOT REMOVED: $sledgeCopy" }
    }
    $left = @($installed | Where-Object { Test-Path $_ })
    if ($left.Count) { Write-Output "NOT REMOVED: $($left -join ', ')" } else { Write-Output "removed the $($installed.Count) installed files" }
    $pkgDir = Join-Path $run 'package'
    if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force -ErrorAction SilentlyContinue }  # the package's own copy
}

if ($BlockSaveWrites) {
    $savesAfter = @(Get-SaveHashes)
    if (($savesBefore -join ';') -ne ($savesAfter -join ';')) { Write-Output 'SAVE FILES CHANGED although blocked (backups are in runs\save_backups)'; exit 5 }
    Write-Output "save files verified untouched ($($savesAfter.Count) files)"
}

if ($Record -and $physx) {  # the recordings this session made, with their marks
    $made = @(Get-ChildItem (Join-Path $root 'recordings') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.CreationTime -ge $sessionStart } | Sort-Object Name)
    Write-Output "recordings made: $($made.Count)"
    foreach ($r in $made) {
        $m = Join-Path $r.FullName 'manifest.json'
        if (Test-Path $m) {
            $j = Get-Content $m -Raw | ConvertFrom-Json
            Write-Output ("  $($r.Name): $($j.frames) frames, $([Math]::Round([double]$j.game_seconds)) s, marks at frames " +
                $(if (@($j.markers).Count) { (@($j.markers) | ForEach-Object { "$_" }) -join ', ' } else { '(none)' }))
        } else {
            Write-Output "  $($r.Name): no manifest (unfinished)"
        }
    }
}

$log = Join-Path $run 'rfgrt.log'
if (-not (Test-Path $log)) { Write-Output 'no rfgrt.log: the path tracer did not load'; exit 2 }
Write-Output '--- rfgrt.log (tail) ---'
Get-Content $log -Tail 30
if (Select-String -Path $log -Pattern 'session end' -Quiet) { exit 0 }
Write-Output 'the log has no session end: the game crashed or was ended (see the crash reports in the run folder)'
exit 1
