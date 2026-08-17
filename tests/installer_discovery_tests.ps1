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

Write-Output 'installer_discovery_tests passed'
