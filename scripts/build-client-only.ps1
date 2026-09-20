# 只编译 ERF 客户端（本次改动仅 C# 客户端；C++ DLL 未变，不必重编）。
# 用法（在装有 .NET SDK 的机器上，例如 Win10 虚拟机）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .	mp\explore-remote-filesuild-client-only.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out  = Join-Path $root 'dist\client-only'
Write-Host "==> publish RemoteFsClient -> $out"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
& dotnet publish (Join-Path $root 'src-client\RemoteFsClient\RemoteFsClient.csproj') `
    -c Release -r win-x64 --self-contained true -o $out
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }
$exe = Join-Path $out 'RemoteFsClient.exe'
if (-not (Test-Path $exe)) { throw "RemoteFsClient.exe missing in $out" }
$fi = Get-Item $exe
Write-Host ("OK: {0} ({1} bytes, {2})" -f $exe, $fi.Length, $fi.LastWriteTime)
