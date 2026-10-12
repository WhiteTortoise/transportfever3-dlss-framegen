# Downloads NVIDIA Streamline (pinned version and SHA-256) into external\.
# Visual Studio Build Tools (C++ workload) and the Vulkan SDK must be installed separately.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = '2.14.1'
$sha256 = '92C4D954631A1710DA86CA3FA8D5034F2B9503838C95FC4AE977AE149319781B'
$zipName = "streamline-sdk-v$version.zip"
$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/$zipName"

$dl = Join-Path $root 'external\dl'
$zip = Join-Path $dl $zipName
$target = Join-Path $root 'external\streamline'
New-Item -ItemType Directory -Force $dl | Out-Null

if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha256) {
    Write-Host "Downloading $url"
    curl.exe -fSL -o $zip $url
    if ($LASTEXITCODE) { throw 'Download failed' }
}
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha256) {
    Remove-Item $zip
    throw 'Checksum mismatch: the download is not the expected Streamline release'
}
if (-not (Test-Path (Join-Path $target 'include\sl.h'))) {
    Write-Host "Extracting to $target"
    Expand-Archive -Path $zip -DestinationPath $target -Force
}
Write-Host "Streamline $version ready in $target"
