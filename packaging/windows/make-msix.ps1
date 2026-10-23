<#
.SYNOPSIS
  Creates an MSIX package of TableSkim for the Microsoft Store.

.DESCRIPTION
  Stages the "runtime" install component of a Release build, generates the
  required logo assets from src/images/icon.png, fills in AppxManifest.xml
  and packs everything with makeappx.exe (Windows SDK).

  Values for -IdentityName, -Publisher and -PublisherDisplayName come from
  Partner Center (Product management > Product identity). The Store signs the
  package itself on submission, so the output is unsigned unless -PfxPath is
  given (only needed for local sideload testing; the certificate subject must
  equal -Publisher).

.EXAMPLE
  .\make-msix.ps1 -IdentityName 12345Name.TableSkim `
      -Publisher "CN=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" `
      -PublisherDisplayName "Your Name"
#>
param(
    [Parameter(Mandatory)] [string] $IdentityName,
    [Parameter(Mandatory)] [string] $Publisher,
    [Parameter(Mandatory)] [string] $PublisherDisplayName,
    [string] $BuildDir = (Join-Path $PSScriptRoot "..\..\build\vs-release"),
    [string] $Version,                      # default: the CMake project version; MSIX needs four parts and the Store reserves the 4th (must be 0)
    [string] $PfxPath,
    [string] $PfxPassword
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BuildDir = Resolve-Path $BuildDir
if (-not $Version) {
    # Follow project(VERSION ...) so that every release gets a higher package version.
    $line = Select-String -Path (Join-Path $BuildDir "CMakeCache.txt") -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$'
    if (-not $line) { throw "CMAKE_PROJECT_VERSION not found in $BuildDir\CMakeCache.txt; pass -Version" }
    $parts = @($line.Matches[0].Groups[1].Value.Trim().Split('.') | Select-Object -First 3)
    while ($parts.Count -lt 3) { $parts += "0" }
    $Version = ($parts + "0") -join "."
}
$stage = Join-Path $BuildDir "msix-stage"
$out = Join-Path $BuildDir "TableSkim-$Version-x64.msix"

function Find-SdkTool([string] $name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $tool = Get-ChildItem $kits -Recurse -Filter $name -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\" } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw "$name not found. Install the Windows 10/11 SDK." }
    return $tool.FullName
}
$makeappx = Find-SdkTool "makeappx.exe"

# 1. Stage the application files (TableSkim.exe, CRT DLLs, LICENSE, ...)
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
cmake --install $BuildDir --config Release --component runtime --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

# 2. Logo assets
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $stage "Assets"
New-Item -ItemType Directory -Force $assets | Out-Null
$src = [System.Drawing.Image]::FromFile((Join-Path $root "src\images\icon.png"))
function New-Logo([string] $name, [int] $w, [int] $h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = "HighQualityBicubic"
    $g.SmoothingMode = "HighQuality"
    $g.PixelOffsetMode = "HighQuality"
    $g.Clear([System.Drawing.Color]::Transparent)
    $side = [Math]::Min($w, $h)
    $g.DrawImage($src, [int](($w - $side) / 2), [int](($h - $side) / 2), $side, $side)
    $g.Dispose()
    $bmp.Save((Join-Path $assets $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
New-Logo "StoreLogo.png" 50 50
New-Logo "Square44x44Logo.png" 44 44
New-Logo "Square44x44Logo.targetsize-44_altform-unplated.png" 44 44
New-Logo "Square150x150Logo.png" 150 150
$src.Dispose()

# 3. Manifest
$manifest = Get-Content (Join-Path $PSScriptRoot "AppxManifest.xml.in") -Raw
$manifest = $manifest.Replace("@MSIX_IDENTITY_NAME@", $IdentityName).
    Replace("@MSIX_PUBLISHER@", [System.Security.SecurityElement]::Escape($Publisher)).
    Replace("@MSIX_PUBLISHER_DISPLAY_NAME@", [System.Security.SecurityElement]::Escape($PublisherDisplayName)).
    Replace("@MSIX_VERSION@", $Version)
Set-Content (Join-Path $stage "AppxManifest.xml") $manifest -Encoding UTF8

# 4. Pack (and optionally sign for local testing)
if (Test-Path $out) { Remove-Item $out -Force }
& $makeappx pack /d $stage /p $out /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

if ($PfxPath) {
    $signtool = Find-SdkTool "signtool.exe"
    & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $out
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
}
Write-Host "Created $out"
