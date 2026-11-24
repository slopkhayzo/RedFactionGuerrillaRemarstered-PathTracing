<#
.SYNOPSIS
  Downloads the parts of NVIDIA's Streamline SDK the plugin's frame generation uses (DLSS Frame Generation with
  Reflex): the headers, the signed x64 runtime DLLs, the licences and the programming guides.

.DESCRIPTION
  Pinned to one release of https://github.com/NVIDIA-RTX/Streamline (the one the SpaceEngineers-RTHWAccel project pins,
  whose script this is). The release zip is checked against its SHA-256 and the DLLs against NVIDIA's Authenticode
  signature; the files land in third_party/streamline/ under the SDK's own layout and stay out of git (.gitignore).
  Streamline is under the MIT licence (license.txt); the DLSS runtime it loads (nvngx_dlssg.dll) under the NVIDIA RTX
  SDKs licence (bin/x64/nvngx_dlss.license.txt), Reflex under its own (bin/x64/reflex.license.txt).

  -From takes the files from another copy of the same release (a folder laid out as third_party/streamline, such as
  another project's) instead of downloading them: the DLLs' signatures are checked the same way.

.EXAMPLE
  tools\fetch_streamline.ps1
  tools\fetch_streamline.ps1 -From D:\Githubs\SpaceEngineers\SpaceEngineers-RTHWAccel\third_party\streamline
#>
[CmdletBinding()]
param(
    [string]$Tag = 'v2.14.16',
    [string]$Sha256 = '32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B',
    [string]$From = '',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'third_party\streamline'
$asset = "streamline-sdk-$Tag.zip"
$files = @(
    'license.txt',
    '3rd-party-licenses.md',
    'README.md',
    'bin/x64/sl.interposer.dll',
    'bin/x64/sl.common.dll',
    'bin/x64/sl.dlss_g.dll',
    'bin/x64/sl.reflex.dll',
    'bin/x64/sl.pcl.dll',
    'bin/x64/nvngx_dlssg.dll',
    'bin/x64/nvngx_dlss.license.txt',
    'bin/x64/reflex.license.txt',
    'docs/ProgrammingGuide.md',
    'docs/ProgrammingGuideDLSS_G.md',
    'docs/ProgrammingGuideManualHooking.md',
    'docs/ProgrammingGuidePCL.md',
    'docs/ProgrammingGuideReflex.md',
    'docs/DLSS-FG Programming Guide.pdf'
)

$present = -not $Force -and ($files | ForEach-Object { Test-Path (Join-Path $target ($_ -replace '/', '\')) }) -notcontains $false
if ($present) {
    Write-Host "Streamline $Tag files already in $target (-Force fetches them again)."
    return
}

if ($From) {
    # Another copy of the release: the listed files and every header.
    $wanted = @($files) + @(Get-ChildItem (Join-Path $From 'include') -Filter *.h | ForEach-Object { "include/$($_.Name)" })
    foreach ($file in $wanted) {
        $source = Join-Path $From ($file -replace '/', '\')
        if (-not (Test-Path $source)) { throw "$From has no $file." }
        $destination = Join-Path $target ($file -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
        Copy-Item $source $destination -Force
    }
} else {
    # The zip in a folder of its own (downloaded content is not run from).
    $download = Join-Path ([IO.Path]::GetTempPath()) "rfgrt-streamline-$Tag"
    New-Item -ItemType Directory -Force $download | Out-Null
    $zipPath = Join-Path $download $asset
    if (-not (Test-Path $zipPath) -or (Get-FileHash -Algorithm SHA256 $zipPath).Hash -ne $Sha256) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/$Tag/$asset"
        Write-Host "downloading: $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zipPath
    }
    $hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash
    if ($hash -ne $Sha256) {
        throw "$asset has SHA-256 $hash, not the pinned $Sha256."
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = @{}
        foreach ($entry in $zip.Entries) {
            $entries[$entry.FullName] = $entry
        }
        # The headers, all of them (they include each other).
        $wanted = @($files) + @($zip.Entries | Where-Object { $_.FullName -like 'include/*.h' } | ForEach-Object { $_.FullName })
        foreach ($file in $wanted) {
            if (-not $entries.ContainsKey($file)) {
                throw "$asset has no $file."
            }
            $destination = Join-Path $target ($file -replace '/', '\')
            New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[$file], $destination, $true)
        }
    } finally {
        $zip.Dispose()
    }
}

$hashes = New-Object System.Collections.Generic.List[string]
foreach ($file in $wanted) {
    $path = Join-Path $target ($file -replace '/', '\')
    if ($file -like '*.dll') {
        $signature = Get-AuthenticodeSignature $path
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') {
            Remove-Item $path -Force
            throw "$file is not signed by NVIDIA ($($signature.Status), $($signature.SignerCertificate.Subject)): removed."
        }
    }
    $fileHash = (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant()
    $hashes.Add("$fileHash  $((Get-Item $path).Length)  $file")
}
$hashes | Set-Content -Encoding UTF8 (Join-Path $target 'SHA256SUMS.txt')
@"
# NVIDIA Streamline SDK (DLSS Frame Generation)

- Source: https://github.com/NVIDIA-RTX/Streamline, release ``$Tag``, asset ``$asset`` (SHA-256 ``$Sha256``), by
  ``tools/fetch_streamline.ps1``$(if ($From) { " (copied from $From, a copy of the same release)" }). The files stay out of git;
  ``SHA256SUMS.txt`` lists their hashes; the DLLs are checked for NVIDIA's Authenticode signature.
- Licences: Streamline itself MIT (``license.txt``); the DLSS runtime ``nvngx_dlssg.dll`` the NVIDIA RTX SDKs License
  (``bin/x64/nvngx_dlss.license.txt``: NVIDIA GPUs only, attribution, a notice to NVIDIA before a commercial release);
  Reflex its own (``bin/x64/reflex.license.txt``).
- Used for: DLSS Frame Generation of the game's presented frames (the presenter, x64 only).
"@ | Set-Content -Encoding UTF8 (Join-Path $target 'UPSTREAM.md')
Write-Host "Streamline $Tag files in $target ($($wanted.Count) files, DLLs signed by NVIDIA); hashes in SHA256SUMS.txt."
