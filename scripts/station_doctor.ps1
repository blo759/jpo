<#
.SYNOPSIS
    Diagnostic des trois stations CellNet depuis un PC connecté au Wi-Fi d'une
    cellule.

.DESCRIPTION
    Affiche le SSID réellement connecté, puis interroge /api/info et
    /api/health de chaque cellule joignable : identité, pairs vivants,
    compteurs du bus RS485 et nombre d'utilisateurs découverts.

.EXAMPLE
    .\scripts\station_doctor.ps1

.EXAMPLE
    .\scripts\station_doctor.ps1 -Cell A

.EXAMPLE
    .\scripts\station_doctor.ps1 -Loop -IntervalSec 3
#>
[CmdletBinding()]
param(
    [ValidateSet('A', 'B', 'C', 'ALL')][string]$Cell = 'ALL',
    [int]$TimeoutSec = 5,
    [switch]$Loop,
    [int]$IntervalSec = 5
)

$cells = @{
    A = [pscustomobject]@{ Site = 'SITE_A'; Ssid = 'CELL_SITE_A'; Ip = '10.0.0.1' }
    B = [pscustomobject]@{ Site = 'SITE_B'; Ssid = 'CELL_SITE_B'; Ip = '10.1.0.1' }
    C = [pscustomobject]@{ Site = 'SITE_C'; Ssid = 'CELL_SITE_C'; Ip = '10.2.0.1' }
}

function Get-CurrentSsid {
    $output = & netsh wlan show interfaces 2>$null
    $line = $output | Where-Object { $_ -match '^\s*SSID\s*:\s*(.+)$' } | Select-Object -First 1
    if (-not $line) { return $null }
    return ($line -replace '^\s*SSID\s*:\s*', '').Trim()
}

function Show-Station {
    param([pscustomobject]$Target)

    $base = "http://$($Target.Ip):8080"
    try {
        $info = Invoke-RestMethod -Uri "$base/api/info" -TimeoutSec $TimeoutSec
    } catch {
        Write-Host ("  {0,-7} {1,-13} INJOIGNABLE — {2}" -f $Target.Site, $Target.Ssid, $_.Exception.Message) -ForegroundColor Red
        return
    }

    $health = $null
    try {
        $health = Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec $TimeoutSec
    } catch {
        # /api/health est optionnel : on garde les informations de /api/info
    }

    Write-Host ("  {0,-7} {1,-13} node={2} ssid={3} ip={4} users={5}" -f `
            $Target.Site, $Target.Ssid, $info.node, $info.ssid, $info.ip, $info.users) -ForegroundColor Green

    $peers = @($info.peers | ForEach-Object {
            if ($_.alive) { "$($_.site)=ON" } else { "$($_.site)=off" }
        }) -join ' '
    Write-Host ("           pairs       : {0}" -f $peers)

    if ($null -ne $health) {
        $bus = $health.bus
        Write-Host ("           bus RS485   : tx={0} rx={1} crc={2} retries={3} failures={4} doublons={5} jetons={6} alive=0x{7:X}" -f `
                $bus.sent, $bus.received, $bus.crcErrors, $bus.retries, $bus.failures, $bus.duplicates, $bus.tokensReceived, $bus.aliveMask)
        # Fields added by the reliability pass: they are displayed only when the
        # station runs a firmware that exposes them.
        if ($null -ne $bus.queueSize) {
            Write-Host ("           files       : app={0} controle={1} refus={2} controles_perdus={3} accuses_en_attente={4}" -f `
                    $bus.queueSize, $bus.queueControl, $bus.queueDrops, $bus.queueControlDrops, $bus.pendingAcks)
            Write-Host ("           fiabilite   : ackTimeouts={0} fragments_rejetes={1} jetons_revendiques={2} tours_inactifs={3}" -f `
                    $bus.ackTimeouts, $bus.fragmentsRejected, $bus.tokensClaimed, $bus.idleRounds)
        }
        if ($null -ne $health.system) {
            Write-Host ("           systeme     : tas={0} min={1} plus_grand_bloc={2} boucle_max={3} us" -f `
                    $health.system.heap, $health.system.heapMin, $health.system.heapMaxBlock, $health.system.maxLoopUs)
        } else {
            Write-Host ("           tas libre   : {0} octets" -f $health.heap)
        }
    } else {
        Write-Host "           /api/health indisponible" -ForegroundColor Yellow
    }
}

$targets = if ($Cell -eq 'ALL') { @($cells.A, $cells.B, $cells.C) } else { @($cells[$Cell]) }

do {
    Write-Host ""
    Write-Host "=== CellNet diagnostic $(Get-Date -Format 'HH:mm:ss') ===" -ForegroundColor Cyan

    $ssid = Get-CurrentSsid
    if ([string]::IsNullOrWhiteSpace($ssid)) {
        Write-Host "PC non connecté à un Wi-Fi connu (ou permission SSID indisponible)." -ForegroundColor Yellow
    } else {
        Write-Host "PC connecté au réseau : $ssid"
        $known = $cells.Values | Where-Object { $_.Ssid -eq $ssid }
        if (-not $known) {
            Write-Host "Ce SSID n'est pas une cellule CellNet : les adresses 10.x ne seront probablement pas joignables." -ForegroundColor Yellow
        }
    }

    foreach ($target in $targets) {
        Show-Station -Target $target
    }

    if ($Loop) {
        Start-Sleep -Seconds $IntervalSec
    }
} while ($Loop)
