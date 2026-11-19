# Creates (or refreshes) this project's private copy of the game: rfg.exe and the game's own DLLs and settings files
# copied from the installed game, every file of its data folder hard-linked (31 GB not copied; the game only reads
# them), and nothing of any mod (no loader, no Sledge, no rfgphys). Other agents test their own patches in the shared
# game folder at the same time; test runs here install this project's files into the copy and remove them again
# afterwards (tools\run_game.ps1), so neither side's configuration can leak into the other's runs.
# Hard links, not a junction: deleting the copy (even recursively) removes only its own links, never the game's data.
# -Remove deletes the copy.
param(
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered',
    [string]$Sandbox = (Join-Path (Split-Path -Parent $PSScriptRoot) 'game\gog'),
    [switch]$Remove
)

if ($Remove) {
    if (Test-Path $Sandbox) {
        if ((Get-Item (Join-Path $Sandbox 'data') -ErrorAction SilentlyContinue).LinkType -eq 'Junction') {
            [IO.Directory]::Delete((Join-Path $Sandbox 'data'), $false)  # an old junction: the link only
        }
        Remove-Item $Sandbox -Recurse -Force
    }
    Write-Output "removed $Sandbox"
    exit 0
}

if (-not (Test-Path (Join-Path $GameDir 'rfg.exe'))) { Write-Output "rfg.exe not found in $GameDir"; exit 2 }
if (Get-Process -Name rfg -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Sandbox*" }) {
    Write-Output 'the sandbox game is running: close it first'; exit 3
}
New-Item -ItemType Directory -Force $Sandbox | Out-Null

# The game's own top-level files only: the mods' (dinput8.dll is Sledge's, winmm.dll the PhysX project's loader), logs,
# the uninstaller and the 116 MB PDB stay out.
$files = Get-ChildItem $GameDir -File | Where-Object {
    $_.Name -match '^(rfg\.exe|.*\.dll|.*\.padcfg|thqnocfg_.*\.dat|goggame-.*|steam_appid\.txt)$' -and
    $_.Name -notmatch '^(dinput8|winmm|d3d11|dxgi|version)\.dll$'
}
foreach ($f in $files) {
    $dst = Join-Path $Sandbox $f.Name
    if (-not (Test-Path $dst) -or (Get-Item $dst).LastWriteTime -ne $f.LastWriteTime -or (Get-Item $dst).Length -ne $f.Length) {
        Copy-Item $f.FullName $dst -Force
        Write-Output "copied $($f.Name)"
    }
}
$data = Join-Path $Sandbox 'data'
New-Item -ItemType Directory -Force $data | Out-Null
$linked = 0
foreach ($f in Get-ChildItem (Join-Path $GameDir 'data') -File) {
    $dst = Join-Path $data $f.Name
    if (Test-Path $dst) {
        $d = Get-Item $dst
        if ($d.Length -eq $f.Length -and $d.LastWriteTime -eq $f.LastWriteTime) { continue }
        Remove-Item $dst -Force  # the game's file was replaced (an update): link the new one
    }
    New-Item -ItemType HardLink -Path $dst -Target $f.FullName | Out-Null
    $linked++
}
foreach ($d in Get-ChildItem $data -File) {  # files the game no longer has
    if (-not (Test-Path (Join-Path (Join-Path $GameDir 'data') $d.Name))) { Remove-Item $d.FullName -Force }
}
if (-not (Test-Path (Join-Path $data 'misc.vpp_pc'))) { Write-Output 'linking the data files failed'; exit 4 }
@"
This folder is rfgrt's private copy of Red Faction Guerrilla Re-Mars-tered for unattended test runs, made by
tools\sandbox.ps1 from $GameDir. The files in data\ are hard links to the installed game's (no extra disk space; the
game only reads them). tools\run_game.ps1 installs the loader and the plugin here for each run and removes them after.
Delete the whole folder (or run tools\sandbox.ps1 -Remove) to get rid of it: that removes only the links.
"@ | Set-Content -Path (Join-Path $Sandbox 'SANDBOX_README.txt') -Encoding UTF8
Write-Output "sandbox ready: $Sandbox ($((Get-ChildItem $Sandbox -File).Count) files, $((Get-ChildItem $data -File).Count) data files, $linked newly linked)"
exit 0
