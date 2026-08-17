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

function Select-ObsFlightControlsOverlayInstallFolder {
    [OutputType([string])]
    param(
        [string]$InitialPath
    )

    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    try {
        $dialog.Description = 'Select the OBS Studio installation folder.'
        $dialog.ShowNewFolderButton = $false
        if ($InitialPath -and (Test-Path -LiteralPath $InitialPath -PathType Container)) {
            $dialog.SelectedPath = $InitialPath
        }

        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.SelectedPath
        }
    } finally {
        $dialog.Dispose()
    }

    return $null
}

function Resolve-ObsFlightControlsOverlayInstallationCandidate {
    [OutputType([string])]
    param(
        [string[]]$Candidates = @(),

        [Parameter(Mandatory = $true)]
        [scriptblock]$TestInstallation,

        [scriptblock]$SelectFolder
    )

    $validCandidates = @(
        $Candidates |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -Unique
    )

    if ($validCandidates.Count -eq 1) {
        return $validCandidates[0]
    }

    $initialPath = if ($validCandidates.Count -gt 0) { $validCandidates[0] } else { $null }
    $selectedPath = if ($null -ne $SelectFolder) {
        & $SelectFolder $initialPath
    } else {
        Select-ObsFlightControlsOverlayInstallFolder -InitialPath $initialPath
    }

    if ([string]::IsNullOrWhiteSpace($selectedPath)) {
        throw 'No OBS Studio installation was selected. Installation cancelled.'
    }

    $selectedPath = $selectedPath.Trim()
    if (-not (& $TestInstallation $selectedPath)) {
        throw "The selected folder is not a valid OBS Studio installation: $selectedPath"
    }

    return $selectedPath
}
