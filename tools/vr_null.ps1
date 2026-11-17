# Casque simulé pour tester sans casque : SteamVR avec son pilote « null » (fourni avec SteamVR),
# dans une configuration séparée (build\vr_null\config). Les réglages SteamVR de l'utilisateur
# (Steam\config\steamvr.vrsettings) ne sont ni lus ni modifiés : VR_CONFIG_PATH / VR_LOG_PATH
# déplacent la configuration et les journaux pour ce lancement seulement.
#
#   powershell -ExecutionPolicy Bypass -File tools\vr_null.ps1 start    # démarre SteamVR simulé
#   powershell -ExecutionPolicy Bypass -File tools\vr_null.ps1 stop     # l'arrête
#
# Le jeu doit être lancé avec les mêmes variables VR_CONFIG_PATH / VR_LOG_PATH (smoke_test.ps1 -Vr
# le fait). Casque simulé : 1512x1680 par œil, 90 Hz, tête immobile, pas de manettes.
# Code de sortie : 0 = bon, 1 = problème (détail affiché).
param([Parameter(Mandatory)][ValidateSet("start", "stop")][string]$Action)

$root = Join-Path (Split-Path -Parent $PSScriptRoot) "build\vr_null"
$config = Join-Path $root "config"
$logs = Join-Path $root "logs"
$marker = Join-Path $root "vrserver.pid"
$names = "vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup"

# Dossier de SteamVR : celui que déclare OpenVR
function Get-SteamVR {
    $vrpath = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (-not (Test-Path $vrpath)) { return $null }
    $runtime = (Get-Content $vrpath -Raw | ConvertFrom-Json).runtime | Select-Object -First 1
    if ($runtime -and (Test-Path (Join-Path $runtime "drivers\null"))) { return $runtime }
    return $null
}

if ($Action -eq "stop") {
    $server = Get-Process vrserver -ErrorAction SilentlyContinue
    $ours = (Test-Path $marker) -and $server -and ($server.Id -contains [int](Get-Content $marker))
    if ($server -and -not $ours) {
        Write-Output "ECHEC : le SteamVR en cours n'a pas ete lance par ce script, il n'est pas arrete"
        exit 1
    }
    Get-Process vrmonitor -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    for ($i = 0; $i -lt 10 -and (Get-Process vrserver -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
    Get-Process $names -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Test-Path $marker) { Remove-Item $marker -Confirm:$false }
    Write-Output "BON : SteamVR simule arrete"
    exit 0
}

if (Get-Process vrserver, vrmonitor -ErrorAction SilentlyContinue) {
    Write-Output "ECHEC : SteamVR tourne deja, ferme-le avant (ou : vr_null.ps1 stop)"
    exit 1
}
$steamvr = Get-SteamVR
if (-not $steamvr) {
    Write-Output "ECHEC : SteamVR (avec son pilote null) introuvable"
    exit 1
}
$active = (Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1" -ErrorAction SilentlyContinue).ActiveRuntime
if ($active -notlike "*steamxr*") {
    Write-Output "ATTENTION : le runtime OpenXR actif n'est pas SteamVR ($active) : lancer le jeu avec --steamvr"
}

New-Item -ItemType Directory -Force $config, $logs | Out-Null
# Réécrit à chaque démarrage : SteamVR y ajoute ses propres clés, on repart d'un état connu.
# power : sans cela le casque simulé (jamais « porté ») passe en veille après 5 s et la session
# OpenXR reste en SYNCHRONIZED, sans image.
@'
{
   "steamvr" : {
      "requireHmd" : false,
      "forcedDriver" : "null",
      "activateMultipleDrivers" : false,
      "enableHomeApp" : false,
      "showMirrorView" : false,
      "startMonitorFromAppLaunch" : false
   },
   "driver_null" : {
      "enable" : true,
      "windowX" : 0,
      "windowY" : 0,
      "windowWidth" : 640,
      "windowHeight" : 360,
      "renderWidth" : 1512,
      "renderHeight" : 1680,
      "displayFrequency" : 90.0
   },
   "power" : {
      "turnOffScreensTimeout" : 86400.0,
      "pauseCompositorOnStandby" : false
   },
   "dashboard" : {
      "enableDashboard" : false
   }
}
'@ | Out-File (Join-Path $config "steamvr.vrsettings") -Encoding ascii

$env:VR_CONFIG_PATH = $config
$env:VR_LOG_PATH = $logs
$serverLog = Join-Path $logs "vrserver.txt"
$start = Get-Date
Start-Process (Join-Path $steamvr "bin\win64\vrstartup.exe")

$ready = $false
for ($i = 0; $i -lt 30 -and -not $ready; $i++) {
    Start-Sleep -Seconds 2
    if ((Test-Path $serverLog) -and (Get-Item $serverLog).LastWriteTime -ge $start) {
        $ready = [bool](Select-String -Path $serverLog -Pattern "Active HMD set to null" -Quiet) -and
            [bool](Get-Process vrcompositor -ErrorAction SilentlyContinue)
    }
}
$server = Get-Process vrserver -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $ready -or -not $server) {
    Write-Output "ECHEC : SteamVR n'a pas active le casque simule (voir $serverLog)"
    exit 1
}
"$($server.Id)" | Out-File $marker -Encoding ascii
Start-Sleep -Seconds 5 # le compositeur finit de démarrer
Write-Output "BON : SteamVR simule pret (pilote null, configuration $config)"
exit 0
