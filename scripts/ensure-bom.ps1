param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$files = Get-ChildItem -Path $root -Recurse -Filter *.ps1 -File |
    Where-Object { $_.FullName -notmatch '\\(bin|obj|dist|\.git)\\' }

foreach ($f in $files) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $nonAscii = $false
    foreach ($b in $bytes) { if ($b -gt 127) { $nonAscii = $true; break } }
    if (-not $nonAscii) { continue }

    if ($hasBom) { Write-Output "OK   (BOM)  $($f.FullName)"; continue }
    Write-Output "MISS (no BOM) $($f.FullName)"
    if ($Apply) {
        # PowerShell 5.1 reads a BOM-less .ps1 as ANSI: a Chinese comment line then
        # eats the following line (measured twice: a swallowed link.exe call made a
        # build silently produce no DLL). Rewrite with BOM, newline bytes untouched.
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        [IO.File]::WriteAllText($f.FullName, $text, (New-Object Text.UTF8Encoding($true)))
        Write-Output "     -> BOM written"
    }
}
