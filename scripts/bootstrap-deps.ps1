# Fetches the pinned vcpkg ports tree and vcpkg tool into .deps/ (idempotent).
# Usage: pwsh -File scripts/bootstrap-deps.ps1     -> prints the vcpkg root on success
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Pinned versions (docs/09-BUILD.md). Changing them is a deliberate dependency update.
$Commit = '0699a19d0c6386247ce50d4dedbb8217d484d536'
$TreeSha256 = 'e56cee05fb300ed57039cceb0f05705bd47d062c8174c6dbd5524e0a4c6d2b0b'
$ToolTag = '2026-09-26'
$ToolSha256 = '2a4ae1467b9e797964c60c919d057b9e5dc1ec03f4b9632c1150026db25201f2'

$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'
$downloads = Join-Path $deps 'dl'
$vcpkgRoot = Join-Path $deps "vcpkg-$Commit"
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

function Get-Verified([string]$url, [string]$file, [string]$sha256) {
    if (-not (Test-Path $file)) {
        Write-Host "downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
    }
    $actual = (Get-FileHash -Algorithm SHA256 -Path $file).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        Remove-Item $file
        throw "SHA-256 mismatch for $url`n  expected $sha256`n  actual   $actual`n" +
              'GitHub may have re-compressed the archive; verify the content and update the pin.'
    }
}

if (-not (Test-Path (Join-Path $vcpkgRoot 'ports'))) {
    $archive = Join-Path $downloads "vcpkg-$Commit.tar.gz"
    Get-Verified "https://codeload.github.com/microsoft/vcpkg/tar.gz/$Commit" $archive $TreeSha256
    # The Windows tar explicitly: Git's MSYS tar (first in PATH under Git Bash) reads "D:\..."
    # as a remote host name.
    & "$env:SystemRoot\System32\tar.exe" -xzf $archive -C $deps
    if ($LASTEXITCODE -ne 0) { throw "extracting $archive failed" }
}

$tool = Join-Path $vcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $tool)) {
    $download = Join-Path $downloads "vcpkg-$ToolTag.exe"
    Get-Verified "https://github.com/microsoft/vcpkg-tool/releases/download/$ToolTag/vcpkg.exe" $download $ToolSha256
    Copy-Item $download $tool
}

# Disable telemetry for every vcpkg call made from this tree.
New-Item -ItemType File -Force -Path (Join-Path $vcpkgRoot 'vcpkg.disable-metrics') | Out-Null
Write-Output $vcpkgRoot
