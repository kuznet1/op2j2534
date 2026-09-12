<#
.SYNOPSIS
    Install and register the OpenPort 2.0 J2534 DLL.

.DESCRIPTION
    Copies the built DLLs to an install directory and registers them under
    PassThruSupport.04.04 so any J2534 application can find them.

    32-bit applications (including TCMFlasher) read
        HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04
    64-bit applications read
        HKLM\SOFTWARE\PassThruSupport.04.04

    Must be run from an elevated PowerShell.

.PARAMETER Uninstall
    Remove the registration and the installed files.

.PARAMETER RemoveStaleTactrix
    Also remove the vendor's PassThruSupport entry if it points at a DLL that
    is not present. A failed Tactrix install leaves such an entry behind, and
    it shows up in device pickers as an adapter that can never open.
#>
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$RemoveStaleTactrix
)

$ErrorActionPreference = 'Stop'

$KeyName   = 'OpenPort 2.0 (CDC)'
$InstallDir = Join-Path ${env:ProgramFiles(x86)} 'OpenportJ2534'
$Root32 = 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04'
$Root64 = 'HKLM:\SOFTWARE\PassThruSupport.04.04'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must be run from an elevated PowerShell."
    }
}

function Remove-Registration {
    foreach ($root in @($Root32, $Root64)) {
        $p = Join-Path $root $KeyName
        if (Test-Path $p) {
            Remove-Item $p -Recurse -Force
            Write-Host "removed $p"
        }
    }
}

function Write-Registration([string]$root, [string]$dll) {
    if (-not (Test-Path $root)) { New-Item -Path $root -Force | Out-Null }
    $p = Join-Path $root $KeyName
    if (-not (Test-Path $p)) { New-Item -Path $p -Force | Out-Null }

    New-ItemProperty $p -Name 'Vendor'            -Value 'OpenportJ2534'      -PropertyType String -Force | Out-Null
    New-ItemProperty $p -Name 'Name'              -Value $KeyName             -PropertyType String -Force | Out-Null
    New-ItemProperty $p -Name 'FunctionLibrary'   -Value $dll                 -PropertyType String -Force | Out-Null
    New-ItemProperty $p -Name 'ConfigApplication' -Value ''                   -PropertyType String -Force | Out-Null
    New-ItemProperty $p -Name 'APIVersion'        -Value '04.04'              -PropertyType String -Force | Out-Null
    New-ItemProperty $p -Name 'ProductVersion'    -Value '1.0.0'              -PropertyType String -Force | Out-Null

    # Protocols this hardware actually supports. J1850 and SW-CAN are absent
    # deliberately: the OP2 firmware rejects J1850, and the vendor's own
    # header documents SW-CAN as unsupported.
    foreach ($proto in @('CAN','ISO15765','ISO9141','ISO14230')) {
        New-ItemProperty $p -Name $proto -Value 1 -PropertyType DWord -Force | Out-Null
    }
    foreach ($proto in @('J1850PWM','J1850VPW','SCI_A_ENGINE','SCI_A_TRANS',
                         'SCI_B_ENGINE','SCI_B_TRANS')) {
        New-ItemProperty $p -Name $proto -Value 0 -PropertyType DWord -Force | Out-Null
    }
    Write-Host "registered $p -> $dll"
}

Assert-Admin

if ($Uninstall) {
    Remove-Registration
    if (Test-Path $InstallDir) {
        Remove-Item $InstallDir -Recurse -Force
        Write-Host "removed $InstallDir"
    }
    Write-Host "`nUninstalled."
    return
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src32 = Join-Path $here 'build\x86\op2j2534.dll'
$src64 = Join-Path $here 'build\x64\op2j2534_64.dll'
if (-not (Test-Path $src32)) { throw "missing $src32 - run build.bat first" }

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item $src32 $InstallDir -Force
$dst32 = Join-Path $InstallDir 'op2j2534.dll'
Write-Host "installed $dst32"
Write-Registration $Root32 $dst32

if (Test-Path $src64) {
    Copy-Item $src64 $InstallDir -Force
    $dst64 = Join-Path $InstallDir 'op2j2534_64.dll'
    Write-Host "installed $dst64"
    Write-Registration $Root64 $dst64
}

if ($RemoveStaleTactrix) {
    foreach ($root in @($Root32, $Root64)) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root | ForEach-Object {
            $props = Get-ItemProperty $_.PSPath
            $lib = $props.FunctionLibrary
            if ($lib -and -not (Test-Path $lib) -and $_.PSChildName -ne $KeyName) {
                Write-Host "stale entry '$($_.PSChildName)' -> missing $lib"
                Remove-Item $_.PSPath -Recurse -Force
                Write-Host "  removed"
            }
        }
    }
}

Write-Host "`nDone. Registered J2534 devices:"
foreach ($root in @($Root32, $Root64)) {
    if (-not (Test-Path $root)) { continue }
    Write-Host "  [$root]"
    Get-ChildItem $root | ForEach-Object {
        $props = Get-ItemProperty $_.PSPath
        $exists = if ($props.FunctionLibrary -and (Test-Path $props.FunctionLibrary)) { 'ok' } else { 'MISSING DLL' }
        Write-Host ("    {0,-42} {1}" -f $_.PSChildName, $exists)
    }
}
