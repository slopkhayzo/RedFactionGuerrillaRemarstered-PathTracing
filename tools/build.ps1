# Configures and builds both architectures with the Visual Studio 2026 toolchain:
#   build-x86\<Config>\rfgrt.asi                      the plugin for today's 32-bit rfg.exe
#   build-x64\<Config>\rfgrt_host.exe                 the 64-bit ray tracing process it starts
#   build-x64\<Config>\rfgrt.asi                      the plugin for a 64-bit rfg.exe (core in-process)
# Run it through the shared queue (not atomic): queue.ps1 submit -Name '...' -Owner RedFactionGuerrillaRemarstered-RTHWAccel -File tools\build.ps1
param([string]$Config = 'RelWithDebInfo', [switch]$X86Only)

$root = Split-Path -Parent $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

$archs = if ($X86Only) { @('Win32') } else { @('Win32', 'x64') }
foreach ($arch in $archs) {
    $dir = Join-Path $root $(if ($arch -eq 'Win32') { 'build-x86' } else { 'build-x64' })
    & $cmake -S $root -B $dir -G 'Visual Studio 18 2026' -A $arch | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "configure $arch failed"; exit $LASTEXITCODE }
    & $cmake --build $dir --config $Config --parallel -- /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { Write-Output "build $arch failed"; exit $LASTEXITCODE }
}
Get-ChildItem (Join-Path $root 'build-x86'), (Join-Path $root 'build-x64') -Recurse -Include rfgrt.asi, rfgrt_host.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\$Config\\" } | Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize | Out-String -Width 200 | Write-Output
exit 0
