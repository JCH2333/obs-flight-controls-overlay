#Requires -Version 5.1
<#
.SYNOPSIS
Installs the packaged OBS Flight Controls Overlay release into OBS Studio.

.DESCRIPTION
Run this script from the root of an extracted release ZIP. It locates a
standard or Steam OBS installation when exactly one is available, then copies
only this plugin's DLL and data files. OBS is never stopped automatically.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$ObsRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$moduleName = 'obs-flight-axis-overlay'
$packageRoot = (Resolve-Path -LiteralPath $PSScriptRoot -ErrorAction Stop).Path
$pluginDll = Join-Path $packageRoot "obs-plugins\64bit\$moduleName.dll"
$pluginData = Join-Path $packageRoot "data\obs-plugins\$moduleName"
$candidates = New-Object System.Collections.Generic.List[string]
$discoveryHelpers = Join-Path $packageRoot 'obs-install-discovery.ps1'
if (-not (Test-Path -LiteralPath $discoveryHelpers -PathType Leaf)) {
    throw "Release discovery helper was not found: $discoveryHelpers"
}
. $discoveryHelpers

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

function Test-ObsInstallation {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }

    return (Test-Path -LiteralPath (Join-Path $Path 'bin\64bit\obs64.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Path 'obs-plugins\64bit') -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $Path 'data\obs-plugins') -PathType Container)
}

function Add-ObsCandidate {
    param([string]$Path)

    if (-not (Test-ObsInstallation -Path $Path)) {
        return
    }

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $alreadyPresent = $false
    foreach ($candidate in $script:candidates) {
        if ([string]::Equals($candidate, $resolved, [System.StringComparison]::OrdinalIgnoreCase)) {
            $alreadyPresent = $true
            break
        }
    }
    if (-not $alreadyPresent) {
        [void]$script:candidates.Add($resolved)
    }
}

function Add-SteamObsCandidates {
    $steamRoots = New-Object System.Collections.Generic.List[string]
    try {
        $steamPath = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steamPath) {
            [void]$steamRoots.Add($steamPath)
        }
    } catch {
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($programFilesX86) {
        [void]$steamRoots.Add((Join-Path $programFilesX86 'Steam'))
    }

    foreach ($steamRoot in $steamRoots | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $steamRoot -PathType Container)) {
            continue
        }

        Add-ObsCandidate -Path (Join-Path $steamRoot 'steamapps\common\OBS Studio')
        $libraryFile = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) {
            continue
        }

        foreach ($line in Get-Content -LiteralPath $libraryFile -ErrorAction Stop) {
            if ($line -notmatch '"path"\s*"(?<path>[^"]+)"') {
                continue
            }
            $libraryRoot = $Matches['path'] -replace '\\\\', '\'
            Add-ObsCandidate -Path (Join-Path $libraryRoot 'steamapps\common\OBS Studio')
        }
    }
}

function Resolve-ObsRoot {
    param([string]$RequestedRoot)

    if ($RequestedRoot) {
        if (-not (Test-ObsInstallation -Path $RequestedRoot)) {
            throw "The selected OBS installation is invalid: $RequestedRoot"
        }
        return (Resolve-Path -LiteralPath $RequestedRoot -ErrorAction Stop).Path
    }

    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    if ($programFiles) {
        Add-ObsCandidate -Path (Join-Path $programFiles 'obs-studio')
    }

    foreach ($registryPath in @(
            'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        $entries = @(Get-ItemProperty -Path $registryPath -ErrorAction SilentlyContinue)
        foreach ($installLocation in @(Get-ObsFlightControlsOverlayRegistryInstallLocations -Entries $entries)) {
            Add-ObsCandidate -Path $installLocation
        }
    }

    Add-SteamObsCandidates

    $selectedRoot = Resolve-ObsFlightControlsOverlayInstallationCandidate `
        -Candidates @($candidates.ToArray()) `
        -TestInstallation {
            param([string]$Path)
            Test-ObsInstallation -Path $Path
        }
    return (Resolve-Path -LiteralPath $selectedRoot -ErrorAction Stop).Path
}

$pluginDllPath = Get-NormalizedPath -Path $pluginDll -Description 'Release plugin DLL'
if ((Split-Path -Leaf $pluginDllPath) -ine "$moduleName.dll") {
    throw "Expected $moduleName.dll, received: $(Split-Path -Leaf $pluginDllPath)"
}

$pluginDataPath = Get-NormalizedPath -Path $pluginData -Description 'Release plugin data directory'
$obsRootPath = Resolve-ObsRoot -RequestedRoot $ObsRoot
$obsExecutable = Join-Path $obsRootPath 'bin\64bit\obs64.exe'

$runningTargetObs = @(Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
    Where-Object {
        -not $_.HasExited -and $_.Path -and
        [string]::Equals($_.Path, $obsExecutable, [System.StringComparison]::OrdinalIgnoreCase)
    })
if ($runningTargetObs.Count -gt 0) {
    throw 'The selected OBS installation is running. Close it and run the installer again.'
}

$pluginDestinationDirectory = Join-Path $obsRootPath 'obs-plugins\64bit'
$dataDestinationRoot = Join-Path $obsRootPath 'data\obs-plugins'
$pluginDestination = Join-Path $pluginDestinationDirectory "$moduleName.dll"
$dataDestination = Join-Path $dataDestinationRoot $moduleName

if (Test-Path -LiteralPath $pluginDestination) {
    Assert-NotReparsePoint -Item (Get-Item -LiteralPath $pluginDestination -Force) -Description 'Plugin DLL destination'
}
if (Test-Path -LiteralPath $dataDestination) {
    Assert-NotReparsePoint -Item (Get-Item -LiteralPath $dataDestination -Force) -Description 'Plugin data destination'
} else {
    New-Item -ItemType Directory -Path $dataDestination -Force | Out-Null
}

Copy-Item -LiteralPath $pluginDllPath -Destination $pluginDestination -Force

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

Write-Host "Installed $moduleName into $obsRootPath"
Write-Host "  DLL:  $pluginDestination"
Write-Host "  Data: $($dataFiles.Count) file(s) in $dataDestination"
