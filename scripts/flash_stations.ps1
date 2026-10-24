<#
.SYNOPSIS
    Compile et téléverse les trois stations avec un mapping explicite
    environnement -> port COM.

.DESCRIPTION
    Evite l'erreur classique qui consiste à flasher site_c sur la carte prévue
    pour SITE_A : chaque environnement est associé à un port nommé, les trois
    ports doivent être différents et présents sur le PC avant tout téléversement.

.EXAMPLE
    .\scripts\flash_stations.ps1 -SiteA COM8 -SiteB COM9 -SiteC COM10

.EXAMPLE
    .\scripts\flash_stations.ps1 -SiteA COM8 -SiteB COM9 -SiteC COM10 -SkipBuild
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SiteA,
    [Parameter(Mandatory = $true)][string]$SiteB,
    [Parameter(Mandatory = $true)][string]$SiteC,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$pio = $null
$candidate = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
if (Test-Path $candidate) {
    $pio = $candidate
} elseif (Get-Command pio -ErrorAction SilentlyContinue) {
    $pio = 'pio'
} else {
    throw "PlatformIO introuvable. Installez-le : python -m pip install --upgrade platformio"
}
Write-Host "PlatformIO : $pio"

$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($availablePorts.Count -eq 0) {
    throw "Aucun port série détecté. Branchez une carte et vérifiez son pilote USB-UART."
}

$mapping = [ordered]@{
    'site_a' = $SiteA
    'site_b' = $SiteB
    'site_c' = $SiteC
}

$unique = $mapping.Values | Select-Object -Unique
if ($unique.Count -ne 3) {
    throw "Les trois ports doivent être différents. Reçu : $($mapping.Values -join ', ')"
}
foreach ($entry in $mapping.GetEnumerator()) {
    if ($availablePorts -notcontains $entry.Value) {
        throw "Port $($entry.Value) absent pour $($entry.Key). Ports détectés : $($availablePorts -join ', ')"
    }
}

foreach ($entry in $mapping.GetEnumerator()) {
    $envName = $entry.Key
    $port = $entry.Value

    Write-Host ""
    Write-Host "=== $envName -> $port ===" -ForegroundColor Cyan

    if (-not $SkipBuild) {
        & $pio run -e $envName
        if ($LASTEXITCODE -ne 0) {
            throw "Compilation échouée pour $envName (voir la sortie ci-dessus)."
        }
    }

    & $pio run -e $envName -t upload --upload-port $port
    if ($LASTEXITCODE -ne 0) {
        throw @"
Téléversement échoué pour $envName sur $port.
  1. Fermez le moniteur série éventuellement ouvert sur ce port.
  2. Maintenez BOOT au moment de 'Connecting....'.
  3. Essayez un autre câble USB, branché directement au PC (sans hub).
  4. Le projet téléverse à 115200 bauds (upload_speed) pour rester fiable.
"@
    }

    Write-Host "OK : $envName flashé sur $port" -ForegroundColor Green
}

Write-Host ""
Write-Host "Téléversement terminé. Contrôlez le SSID affiché au démarrage :" -ForegroundColor Yellow
Write-Host "  site_a  ->  CELL_SITE_A / SITE_A / node 1"
Write-Host "  site_b  ->  CELL_SITE_B / SITE_B / node 2"
Write-Host "  site_c  ->  CELL_SITE_C / SITE_C / node 3"
Write-Host ""
Write-Host "Moniteur série, par exemple :" -ForegroundColor Yellow
Write-Host "  & `"$env:USERPROFILE\.platformio\penv\Scripts\pio.exe`" device monitor --port $SiteA --baud 115200"
