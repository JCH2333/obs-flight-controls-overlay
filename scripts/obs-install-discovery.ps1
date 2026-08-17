function Get-ObsFlightControlsOverlayOptionalStringProperty {
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $null
    }

    $value = ([string]$property.Value).Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $null
    }

    return $value
}

function Get-ObsFlightControlsOverlayRegistryInstallLocations {
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Entries
    )

    foreach ($entry in $Entries) {
        if ($null -eq $entry) {
            continue
        }

        $displayName = Get-ObsFlightControlsOverlayOptionalStringProperty -Object $entry -Name 'DisplayName'
        $installLocation = Get-ObsFlightControlsOverlayOptionalStringProperty -Object $entry -Name 'InstallLocation'
        if ($displayName -and $displayName -like 'OBS Studio*' -and $installLocation) {
            Write-Output $installLocation
        }
    }
}
