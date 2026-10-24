<#
.SYNOPSIS
    Capture et horodate les journaux série des stations CellNet (diagnostic RS485).

.DESCRIPTION
    Ouvre un ou plusieurs ports série à 115200 bauds et affiche chaque ligne reçue
    avec un horodatage commun à tous les ports : les événements de deux stations
    peuvent ainsi être corrélés, ce qui permet de distinguer « la station ne reçoit
    aucun octet » de « la station reçoit mais ne décode aucune trame ».

    L'option -Reset réinitialise la carte en jouant sur DTR/RTS exactement comme le
    fait esptool pour lancer le moniteur. Elle ne flashe rien et n'efface rien :
    elle permet simplement de relire la bannière de démarrage (identifiant du nœud,
    SSID, adresse IP, terminaison, capacités des files).

.PARAMETER Ports
    Un ou plusieurs ports, par exemple COM8,COM11.

.PARAMETER Seconds
    Durée de capture en secondes (défaut 15).

.PARAMETER Reset
    Réinitialise les cartes avant la capture (DTR/RTS), sans rien téléverser.

.PARAMETER Send
    Caractères envoyés sur le port après l'ouverture, par exemple « d » pour le
    résumé de bus ou « dp » pour le résumé puis les pairs et les routes. Le
    firmware interprète ces commandes en lecture seule (voir la console série).

.PARAMETER OutFile
    Fichier texte de sortie (la sortie console est toujours produite).

.EXAMPLE
    .\scripts\serial_capture.ps1 -Ports COM8,COM11 -Seconds 25 -Reset

.EXAMPLE
    .\scripts\serial_capture.ps1 -Ports COM11 -Seconds 5 -Send dp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Ports,
    [int]$Seconds = 15,
    [int]$Baud = 115200,
    [switch]$Reset,
    [string]$Send,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'

$available = [System.IO.Ports.SerialPort]::GetPortNames()
foreach ($port in $Ports) {
    if ($available -notcontains $port) {
        throw "Port $port absent. Ports détectés : $($available -join ', ')"
    }
}

$serialPorts = @{}
foreach ($port in $Ports) {
    $sp = New-Object System.IO.Ports.SerialPort $port, $Baud, 'None', 8, 'One'
    # A busy station can emit more than the default 4 kB driver buffer holds
    # between two reads: a bigger buffer avoids losing the beginning of lines.
    $sp.ReadBufferSize = 65536
    $sp.ReadTimeout = 60
    $sp.NewLine = "`n"
    $sp.Open()
    $serialPorts[$port] = $sp
}

if ($Reset) {
    # Same sequence as esptool's monitor: assert DTR (GPIO0 stays high) then pulse
    # RTS (EN low) to reboot the chip in normal mode.
    foreach ($port in $Ports) {
        $sp = $serialPorts[$port]
        $sp.DtrEnable = $false
        $sp.RtsEnable = $true
    }
    Start-Sleep -Milliseconds 150
    foreach ($port in $Ports) {
        $serialPorts[$port].RtsEnable = $false
    }
    Start-Sleep -Milliseconds 50
}

$buffer = New-Object System.Text.StringBuilder
$watch = [Diagnostics.Stopwatch]::StartNew()

if ($Send) {
    # The firmware serial console reads single characters: they are sent one by
    # one so each command is processed separately.
    foreach ($port in $Ports) {
        foreach ($character in $Send.ToCharArray()) {
            $serialPorts[$port].Write($character.ToString())
            Start-Sleep -Milliseconds 120
        }
    }
    Start-Sleep -Milliseconds 200
}

Write-Host ("=== capture {0} s sur {1} ===" -f $Seconds, ($Ports -join ', ')) -ForegroundColor Cyan
$deadline = $watch.Elapsed.TotalSeconds + $Seconds
while ($watch.Elapsed.TotalSeconds -lt $deadline) {
    $received = $false
    foreach ($port in $Ports) {
        $sp = $serialPorts[$port]
        try {
            # ReadLine is line oriented and cheap, which keeps the loop able to
            # follow a station that logs several hundred lines per second.
            $line = $sp.ReadLine()
        } catch [System.TimeoutException] {
            continue
        } catch {
            continue
        }
        if ($null -eq $line) { continue }
        $received = $true
        $line = $line.TrimEnd("`r")
        $stamp = '[+{0,7:N3}s]' -f $watch.Elapsed.TotalSeconds
        $text = '{0} {1,-6} | {2}' -f $stamp, $port, $line
        Write-Host $text
        [void]$buffer.AppendLine($text)
    }
    if (-not $received) {
        Start-Sleep -Milliseconds 10
    }
}

foreach ($port in $Ports) {
    $serialPorts[$port].Close()
    $serialPorts[$port].Dispose()
}

if ($OutFile) {
    $buffer.ToString() | Set-Content -Path $OutFile -Encoding UTF8
    Write-Host ("Sortie écrite dans $OutFile") -ForegroundColor Yellow
}
