# Unregister ExplorerRemoteFs namespace extension.
# Usage:  powershell -ExecutionPolicy Bypass -File unregister.ps1 [-BinDir <path>]
# Must run as Administrator.

param(
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"

$guid = "9EADE55A-2AB6-4087-B5FF-D090D38173E6"

if (-not $BinDir) {
    $BinDir = Join-Path $PSScriptRoot "..\src\ExplorerRemoteFs\bin\Debug\net8.0-windows"
}
$BinDir = [System.IO.Path]::GetFullPath($BinDir)
$comhost = Join-Path $BinDir "ExplorerRemoteFs.comhost.dll"

# Remove junction point.
Remove-Item -Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$guid" -Recurse -Force -ErrorAction SilentlyContinue

# Remove class registration.
Remove-Item -Path "HKLM:\Software\Classes\CLSID\$guid" -Recurse -Force -ErrorAction SilentlyContinue

# Unregister comhost if present.
if (Test-Path $comhost) {
    & "$env:SystemRoot\System32\regsvr32.exe" /u /s $comhost
}

Write-Host "Unregistered OK. Restart Explorer to refresh the navigation pane."
