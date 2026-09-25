<#
.SYNOPSIS
    Builds, signs and installs zcr.exe for the current user, and starts it at login.

.DESCRIPTION
    Copies the release build to %LOCALAPPDATA%\Programs\ZCR\zcr.exe, registers
    that path in HKCU\...\Run (zcr --register-autostart), and starts the tray.
    A running copy is asked to quit first so the file can be replaced; if it is
    recording, the recording is finalized before it exits.

.PARAMETER NoSign
    Skip Authenticode signing (for a machine without the signing credentials).
#>

param(
    [switch]$NoSign
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Built = Join-Path $RepoRoot 'build\release\Release\zcr.exe'
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\ZCR'
$Installed = Join-Path $InstallDir 'zcr.exe'

Push-Location $RepoRoot
try {
    cmake --preset release
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build --preset release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
}
finally {
    Pop-Location
}

if (-not $NoSign) {
    & (Join-Path $PSScriptRoot 'sign.ps1') $Built
    if ($LASTEXITCODE -ne 0) { throw "signing failed" }
}

# The installed copy, not whatever zcr is first on PATH, is the one holding the
# file open.
if (Test-Path $Installed) {
    Start-Process -FilePath $Installed -ArgumentList '--quit' -Wait -NoNewWindow | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Process -Name zcr -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $Installed }) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
}

New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item -Force $Built $Installed

# zcr.exe is a GUI-subsystem binary, which PowerShell does not wait for when
# invoked directly, so its exit code would be lost.
$reg = Start-Process -FilePath $Installed -ArgumentList '--register-autostart' -Wait -PassThru -NoNewWindow
if ($reg.ExitCode -ne 0) { throw "--register-autostart failed with exit code $($reg.ExitCode)" }

# User PATH, read and written raw: [Environment]::GetEnvironmentVariable expands
# %VAR% entries, and writing that back would silently flatten them for good.
$envKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
try {
    $raw = [string]$envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $entries = @($raw -split ';' | Where-Object { $_ })
    if (-not ($entries | Where-Object { $_.TrimEnd('\') -ieq $InstallDir })) {
        $kind = if ($raw) { $envKey.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
        $envKey.SetValue('Path', (($entries + $InstallDir) -join ';'), $kind)
        # Tell Explorer so new shells see it without signing out.
        [Environment]::SetEnvironmentVariable('ZCR_PATH_REFRESH', '1', 'User')
        [Environment]::SetEnvironmentVariable('ZCR_PATH_REFRESH', $null, 'User')
        Write-Host "Added $InstallDir to the user PATH. Open a new shell to pick it up."
    }
}
finally {
    $envKey.Close()
}

Start-Process -FilePath $Installed
Write-Host "Installed $Installed and registered it to start at login." -ForegroundColor Green
