[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$SkipTests,
    [switch]$SkipFresh,
    [switch]$CleanScratch,
    # Signs the built layer locally and installs that signed copy instead
    # (Sign-LocalLayer.ps1): for testing in games with anti-cheat only.
    [switch]$Sign,
    [string]$SignNote = "",
    # After the build, also copies the layer's DLL and manifest to this folder
    # (for example one that holds the files to install with another tool).
    # Defaults to the VECTORXR_EXPORT_DIR environment variable; nothing is
    # copied while neither is set.
    [string]$ExportDir = $env:VECTORXR_EXPORT_DIR
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$buildScript = Join-Path $PSScriptRoot "Build-Layer.ps1"
$installScript = Join-Path $PSScriptRoot "Install-Layer.ps1"

$buildArgs = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
}

if ($SkipTests) {
    $buildArgs.SkipTests = $true
}

if ($SkipFresh) {
    $buildArgs.SkipFresh = $true
}

if ($CleanScratch) {
    $buildArgs.CleanScratch = $true
}

& $buildScript @buildArgs
if ($Sign) {
    & (Join-Path $PSScriptRoot "Sign-LocalLayer.ps1") -BuildDir $BuildDir -Configuration $Configuration -Install -Note $SignNote
} else {
    & $installScript -BuildDir $BuildDir -Configuration $Configuration
}

if ($ExportDir) {
    $layerDir = Join-Path (Join-Path (Join-Path $PSScriptRoot "..") $BuildDir) "layer"
    $layerDir = [System.IO.Path]::GetFullPath((Join-Path $layerDir $Configuration))
    New-Item -ItemType Directory -Force -Path $ExportDir | Out-Null
    foreach ($name in @("XR_APILAYER_DIENERTECH_VECTORXR.dll", "XR_APILAYER_DIENERTECH_VECTORXR.json")) {
        Copy-Item -LiteralPath (Join-Path $layerDir $name) -Destination $ExportDir -Force
    }
    Write-Host "Copied the layer's DLL and manifest to $ExportDir"
}
