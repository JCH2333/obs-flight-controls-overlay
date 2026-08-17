Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot '..\scripts\obs-install-discovery.ps1')

function Require {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "FAILED: $Message"
    }
}

$entries = @(
    [pscustomobject]@{ Publisher = 'Missing display name' },
    [pscustomobject]@{ DisplayName = 'Unrelated application'; InstallLocation = 'C:\Apps\Other' },
    [pscustomobject]@{ DisplayName = 'OBS Studio'; InstallLocation = 'C:\Apps\OBS' },
    [pscustomobject]@{ DisplayName = 'OBS Studio Portable'; InstallLocation = '  D:\Portable OBS  ' },
    [pscustomobject]@{ DisplayName = 'OBS Studio'; InstallLocation = '   ' },
    [pscustomobject]@{ DisplayName = 'OBS Studio' }
)

$locations = @(Get-ObsFlightControlsOverlayRegistryInstallLocations -Entries $entries)
Require ($locations.Count -eq 2) 'only complete OBS registry records are returned'
Require ($locations[0] -eq 'C:\Apps\OBS') 'first OBS install location'
Require ($locations[1] -eq 'D:\Portable OBS') 'trimmed OBS install location'

$script:selectionCount = 0
$script:selectedPath = $null
$pickedRoot = Resolve-ObsFlightControlsOverlayInstallationCandidate `
    -Candidates @() `
    -SelectFolder {
        param([string]$InitialPath)
        $script:selectionCount++
        Require ([string]::IsNullOrWhiteSpace($InitialPath)) 'no discovered install starts without a suggested folder'
        return 'D:\Apps\OBS Studio'
    } `
    -TestInstallation {
        param([string]$Path)
        $script:selectedPath = $Path
        return $Path -eq 'D:\Apps\OBS Studio'
    }
Require ($script:selectionCount -eq 1) 'folder picker is used when OBS cannot be discovered'
Require ($script:selectedPath -eq 'D:\Apps\OBS Studio') 'selected folder is validated before use'
Require ($pickedRoot -eq 'D:\Apps\OBS Studio') 'validated picked folder is returned'

$script:multipleInitialPath = $null
$multiRoot = Resolve-ObsFlightControlsOverlayInstallationCandidate `
    -Candidates @('C:\OBS A', 'D:\OBS B') `
    -SelectFolder {
        param([string]$InitialPath)
        $script:multipleInitialPath = $InitialPath
        return 'D:\OBS B'
    } `
    -TestInstallation {
        param([string]$Path)
        return $Path -eq 'D:\OBS B'
    }
Require ($script:multipleInitialPath -eq 'C:\OBS A') 'multiple installs provide the first discovered folder as the picker suggestion'
Require ($multiRoot -eq 'D:\OBS B') 'picker chooses the intended OBS installation'

$invalidSelectionRejected = $false
try {
    Resolve-ObsFlightControlsOverlayInstallationCandidate `
        -Candidates @() `
        -SelectFolder { return 'C:\Not OBS' } `
        -TestInstallation { return $false } | Out-Null
} catch {
    $invalidSelectionRejected = $_.Exception.Message -like 'The selected folder is not a valid OBS Studio installation:*'
}
Require $invalidSelectionRejected 'invalid picker selection is rejected'

$cancellationReported = $false
try {
    Resolve-ObsFlightControlsOverlayInstallationCandidate `
        -Candidates @() `
        -SelectFolder { return $null } `
        -TestInstallation { return $true } | Out-Null
} catch {
    $cancellationReported = $_.Exception.Message -eq 'No OBS Studio installation was selected. Installation cancelled.'
}
Require $cancellationReported 'picker cancellation reports a clear message'

Write-Output 'installer_discovery_tests passed'
