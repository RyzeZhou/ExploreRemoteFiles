# Unregister ExplorerRemoteFs for the current user (no admin needed).
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

Remove-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$guid" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path "HKCU:\Software\Classes\CLSID\$guid" -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Unregistered (current user). Restart Explorer to refresh the navigation pane."
