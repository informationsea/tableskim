<#
.SYNOPSIS
  Creates an MSIX package of TableSkim for the Microsoft Store.

.DESCRIPTION
  Stages the "runtime" install component of a Release build, generates the
  logo assets from src/images/icon*.png and the file type icons from
  src/images/filetypes/*.ico, fills in AppxManifest.xml, indexes the assets
  in resources.pri with makepri.exe and packs everything with makeappx.exe
  (both from the Windows SDK).

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

# 2. Logo assets. Windows picks the variant for the display scale
# (scale-*) or the size it draws at (targetsize-*: taskbar, Start, Explorer)
# through resources.pri (step 4); "unplated" ones are drawn without a
# backplate. Sizes with a pixel-fitted icon (src/images/icon-<n>.png) use it.
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $stage "Assets"
$fileTypeAssets = Join-Path $assets "FileTypes"
New-Item -ItemType Directory -Force $fileTypeAssets | Out-Null
$images = Join-Path $root "src\images"
$icon = [System.Drawing.Image]::FromFile((Join-Path $images "icon.png"))

function Save-Png([System.Drawing.Image] $src, [string] $path, [int] $size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = "HighQualityBicubic"
    $g.SmoothingMode = "HighQuality"
    $g.PixelOffsetMode = "HighQuality"
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $size, $size)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function New-Logo([string] $name, [int] $size) {
    $fitted = Join-Path $images "icon-$size.png"
    if (Test-Path $fitted) {
        Copy-Item $fitted (Join-Path $assets $name)
    } else {
        Save-Png $icon (Join-Path $assets $name) $size
    }
}
$scales = @(100, 125, 150, 200, 400)
foreach ($scale in $scales) {
    New-Logo "StoreLogo.scale-$scale.png" ([int][Math]::Ceiling(50 * $scale / 100))
    New-Logo "Square44x44Logo.scale-$scale.png" ([int][Math]::Ceiling(44 * $scale / 100))
    New-Logo "Square150x150Logo.scale-$scale.png" ([int][Math]::Ceiling(150 * $scale / 100))
}
foreach ($size in @(16, 24, 32, 48, 256)) {
    New-Logo "Square44x44Logo.targetsize-$size.png" $size
    New-Logo "Square44x44Logo.targetsize-$($size)_altform-unplated.png" $size
}
$icon.Dispose()

# File type icons, for each Assets\FileTypes\<name>.png the manifest names:
# the PNG images inside src/images/filetypes/<name>.ico (16 to 256 px, the
# small ones pixel-fitted), written as they are.
function Get-IcoImages([string] $path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $result = @{}
    for ($i = 0; $i -lt [BitConverter]::ToUInt16($bytes, 4); $i++) {
        $entry = 6 + 16 * $i
        $size = if ($bytes[$entry] -eq 0) { 256 } else { [int]$bytes[$entry] }
        $length = [BitConverter]::ToInt32($bytes, $entry + 8)
        $png = New-Object byte[] $length
        [Array]::Copy($bytes, [BitConverter]::ToInt32($bytes, $entry + 12), $png, 0, $length)
        if ($png[0] -ne 0x89 -or $png[1] -ne 0x50) { throw "$path holds an image other than PNG" }
        $result[$size] = $png
    }
    return $result
}
$template = Get-Content (Join-Path $PSScriptRoot "AppxManifest.xml.in") -Raw
$fileTypes = [regex]::Matches($template, 'Assets\\FileTypes\\(\w+)\.png') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
foreach ($name in $fileTypes) {
    $ico = Get-IcoImages (Join-Path $images "filetypes\$name.ico")
    foreach ($size in @(16, 24, 32, 48, 256)) {
        if (-not $ico.ContainsKey($size)) { throw "$name.ico has no $size px image" }
        [System.IO.File]::WriteAllBytes((Join-Path $fileTypeAssets "$name.targetsize-$size.png"), $ico[$size])
    }
    # The scale-100 size of a file type logo (44 px), for lookups by scale.
    $stream = New-Object System.IO.MemoryStream (, $ico[256])
    $large = [System.Drawing.Image]::FromStream($stream)
    Save-Png $large (Join-Path $fileTypeAssets "$name.scale-100.png") 44
    $large.Dispose()
    $stream.Dispose()
}

# 3. Manifest
$manifest = $template.Replace("@MSIX_IDENTITY_NAME@", $IdentityName).
    Replace("@MSIX_PUBLISHER@", [System.Security.SecurityElement]::Escape($Publisher)).
    Replace("@MSIX_PUBLISHER_DISPLAY_NAME@", [System.Security.SecurityElement]::Escape($PublisherDisplayName)).
    Replace("@MSIX_VERSION@", $Version)
$manifestPath = Join-Path $stage "AppxManifest.xml"
Set-Content $manifestPath $manifest -Encoding UTF8

# 4. Resource index (resources.pri), which resolves the qualified logo names
# above. Only Assets is indexed: the PRI's paths are relative to its root,
# which therefore mirrors the stage.
$makepri = Find-SdkTool "makepri.exe"
$priRoot = Join-Path $BuildDir "msix-pri"
$priConfig = Join-Path $BuildDir "msix-priconfig.xml"
if (Test-Path $priRoot) { Remove-Item $priRoot -Recurse -Force }
New-Item -ItemType Directory -Force $priRoot | Out-Null
Copy-Item $assets $priRoot -Recurse
& $makepri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed" }
# The default configuration splits the scale variants into
# resources.scale-*.pri for the resource packages of a bundle; a single
# package is read through resources.pri only, so keep them all in it.
$config = [xml](Get-Content $priConfig -Raw)
foreach ($node in @($config.SelectNodes("//packaging"))) {
    [void]$node.ParentNode.RemoveChild($node)
}
$config.Save($priConfig)
& $makepri new /pr $priRoot /cf $priConfig /mn $manifestPath /of (Join-Path $stage "resources.pri") /o
if ($LASTEXITCODE -ne 0) { throw "makepri new failed" }
$split = Get-ChildItem $stage -Filter "resources.*.pri"
if ($split) { throw "makepri split the resources: $($split.Name -join ', ')" }

# 5. Pack (and optionally sign for local testing)
if (Test-Path $out) { Remove-Item $out -Force }
& $makeappx pack /d $stage /p $out /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

if ($PfxPath) {
    $signtool = Find-SdkTool "signtool.exe"
    & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $out
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
}
Write-Host "Created $out"
