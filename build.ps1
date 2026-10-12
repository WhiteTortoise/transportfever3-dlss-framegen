# Builds winhttp.dll. With -Install, copies it into the game folder (game must be closed).
param([switch]$Install)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$gameDir = Split-Path $root -Parent

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# Import the MSVC x64 environment into this PowerShell session.
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

if (-not $env:VULKAN_SDK) {
    $env:VULKAN_SDK = (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name | Select-Object -Last 1).FullName
}

$build = Join-Path $root 'build'
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DVULKAN_SDK_DIR=$env:VULKAN_SDK"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if ($Install) {
    if (Get-Process TransportFever3 -ErrorAction SilentlyContinue) {
        throw 'Transport Fever 3 is running; close it before installing.'
    }
    Copy-Item (Join-Path $build 'winhttp.dll') $gameDir -Force
    $fgDir = Join-Path $gameDir 'framegen'
    New-Item -ItemType Directory -Force $fgDir | Out-Null
    Copy-Item (Join-Path $build 'framegen\*') $fgDir -Force
    Write-Host "Installed winhttp.dll and framegen\ into $gameDir"
}
