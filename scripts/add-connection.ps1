# Add or update a connection in connections.json (no admin needed).
# Usage:
#   powershell -ExecutionPolicy Bypass -File add-connection.ps1 -Name prod -Type sftp -Host 1.2.3.4 -User deploy -Pass 'xxx'
#   powershell -ExecutionPolicy Bypass -File add-connection.ps1 -Name nas -Type ftp -Host nas.local -User alex -Pass 'yyy' -StartPath /data
# Optional: -Port 22 -Key C:\Users\me\.ssh\id_ed25519

param(
    [Parameter(Mandatory = $true)][string]$Name,
    [ValidateSet("sftp", "ftp", "ftps")][string]$Type = "sftp",
    [Parameter(Mandatory = $true)][string]$Host,
    [int]$Port = 0,
    [string]$User = "",
    [string]$Pass = "",
    [string]$Key = "",
    [string]$StartPath = "/"
)

$ErrorActionPreference = "Stop"

$configDir = Join-Path $env:APPDATA "ExplorerRemoteFs"
$configFile = Join-Path $configDir "connections.json"
if (-not (Test-Path $configDir)) { New-Item -Path $configDir -ItemType Directory -Force | Out-Null }

$conns = @()
if (Test-Path $configFile) {
    $json = Get-Content -Path $configFile -Raw -Encoding UTF8
    if ($json) { $conns = @($json | ConvertFrom-Json) }
}

# remove existing with same name
$conns = @($conns | Where-Object { $_.Name -ne $Name })

$conn = [PSCustomObject]@{
    Name       = $Name
    Type       = $Type
    Host       = $Host
    Username   = $User
    Password   = $Pass
    StartPath  = $StartPath
}
if ($Port -gt 0) { $conn | Add-Member -NotePropertyName Port -NotePropertyValue $Port }
if ($Key) { $conn | Add-Member -NotePropertyName PrivateKeyPath -NotePropertyValue $Key }

$conns += $conn
$conns | ConvertTo-Json -Depth 4 | Set-Content -Path $configFile -Encoding UTF8

Write-Host "Saved connection '$Name' -> $configFile"
Write-Host "Restart Explorer to refresh the connection list."
