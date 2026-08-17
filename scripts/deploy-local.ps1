#Requires -Version 5.1
<#
.SYNOPSIS
Deploys the local Release build of obs-flight-axis-overlay into OBS Studio.

.DESCRIPTION
Copies only obs-flight-axis-overlay.dll and the plugin's own data directory.
It never stops OBS and never deletes existing files.  Deployment is refused
while an OBS process is running so OBS cannot load a partially replaced DLL.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PluginDll,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PluginData,

    [Parameter(Mandatory = $false)]
    [ValidateNotNullOrEmpty()]
    [string]$ObsRoot = 'C:\Program Files\obs-studio',

    [Parameter(Mandatory = $false)]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$moduleName = 'obs-flight-axis-overlay'

function Get-NormalizedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description was not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
}

function Assert-NotReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileSystemInfo]$Item,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description is a symbolic link or junction and will not be overwritten: $($Item.FullName)"
    }
}

if ($Configuration -and $Configuration -ne 'Release') {
    throw "Local OBS deployment accepts Release artifacts only. Build with '--config Release' (received '$Configuration')."
}

$pluginDllPath = Get-NormalizedPath -Path $PluginDll -Description 'Plugin DLL'
if ((Split-Path -Leaf $pluginDllPath) -ine "$moduleName.dll") {
    throw "Expected $moduleName.dll, received: $(Split-Path -Leaf $pluginDllPath)"
}

$pluginDataPath = Get-NormalizedPath -Path $PluginData -Description 'Plugin data directory'
if ((Split-Path -Leaf $pluginDataPath) -ine $moduleName) {
    throw "Plugin data directory must be named '$moduleName': $pluginDataPath"
}

$obsRootPath = Get-NormalizedPath -Path $ObsRoot -Description 'OBS installation root'
$obsExecutable = Join-Path $obsRootPath 'bin\64bit\obs64.exe'
if (-not (Test-Path -LiteralPath $obsExecutable -PathType Leaf)) {
    throw "OBS 64-bit executable was not found under the selected installation: $obsExecutable"
}

$runningTargetObs = @(Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
    Where-Object {
        -not $_.HasExited -and $_.Path -and
        [string]::Equals($_.Path, $obsExecutable, [System.StringComparison]::OrdinalIgnoreCase)
    })
if ($runningTargetObs.Count -gt 0) {
    $processList = ($runningTargetObs | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    throw "The selected OBS installation must be closed before deployment. Running process: $processList"
}

$pluginDestinationDirectory = Join-Path $obsRootPath 'obs-plugins\64bit'
$dataDestinationRoot = Join-Path $obsRootPath 'data\obs-plugins'
if (-not (Test-Path -LiteralPath $pluginDestinationDirectory -PathType Container)) {
    throw "OBS plugin directory was not found: $pluginDestinationDirectory"
}
if (-not (Test-Path -LiteralPath $dataDestinationRoot -PathType Container)) {
    throw "OBS plugin data directory was not found: $dataDestinationRoot"
}

$pluginDestination = Join-Path $pluginDestinationDirectory "$moduleName.dll"
if (Test-Path -LiteralPath $pluginDestination) {
    Assert-NotReparsePoint -Item (Get-Item -LiteralPath $pluginDestination -Force) -Description 'Plugin DLL destination'
}

$dataDestination = Join-Path $dataDestinationRoot $moduleName
if (Test-Path -LiteralPath $dataDestination) {
    Assert-NotReparsePoint -Item (Get-Item -LiteralPath $dataDestination -Force) -Description 'Plugin data destination'
} else {
    New-Item -ItemType Directory -Path $dataDestination -Force | Out-Null
}

# Copy exactly this module's DLL. No other binary or OBS file is touched.
Copy-Item -LiteralPath $pluginDllPath -Destination $pluginDestination -Force

# Copy files one at a time so a future data addition remains constrained to this
# module directory and no stale or unrelated OBS data is removed.
$sourcePrefix = $pluginDataPath + [System.IO.Path]::DirectorySeparatorChar
$dataFiles = @(Get-ChildItem -LiteralPath $pluginDataPath -File -Recurse -Force)
foreach ($sourceFile in $dataFiles) {
    if (-not $sourceFile.FullName.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to copy a file outside the plugin data directory: $($sourceFile.FullName)"
    }

    $relativePath = $sourceFile.FullName.Substring($sourcePrefix.Length)
    if ($relativePath -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "Refusing an unsafe plugin data path: $relativePath"
    }

    $destinationFile = Join-Path $dataDestination $relativePath
    $destinationDirectory = Split-Path -Parent $destinationFile
    if (-not (Test-Path -LiteralPath $destinationDirectory)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    if (Test-Path -LiteralPath $destinationFile) {
        Assert-NotReparsePoint -Item (Get-Item -LiteralPath $destinationFile -Force) -Description 'Plugin data destination file'
    }

    Copy-Item -LiteralPath $sourceFile.FullName -Destination $destinationFile -Force
}

Write-Host "Deployed $moduleName to $obsRootPath"
Write-Host "  DLL:  $pluginDestination"
Write-Host "  Data: $($dataFiles.Count) file(s) in $dataDestination"
