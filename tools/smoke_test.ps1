# Essai de démarrage, sans casque : lance Saints Row 2 (solo) avec la DLL de build\Release, attend,
# vérifie le journal (signatures trouvées, détours posés, images rendues, pas d'erreur), puis
# ferme le jeu. Même principe que tools\smoke_test.ps1 de WarbandVR.
#
#   powershell -ExecutionPolicy Bypass -File tools\smoke_test.ps1 [-Seconds 60] [-Vr]
#       [-Script tests\scripts\X.txt] [-ExpectStereo] [-Set "Display.frame_cap=0"] [-Name nom]
#
# -Vr : démarre d'abord un casque simulé (tools\vr_null.ps1 : SteamVR, pilote « null ») et vérifie
#   en plus que la session OpenXR atteint FOCUSED et que des images du jeu partent vers le casque.
# -Script <fichier> : script d'entrées rejoué par la DLL (manette virtuelle, clavier, captures ;
#   voir src\input\test_script.h), pour aller plus loin que le menu principal.
# -ExpectStereo : avec un script qui entre en partie, exige des paires d'images (vrai relief).
# -Set "Section.cle=valeur,..." : réglages de sr2_vr.ini pour cet essai seulement (le fichier
#   d'origine est remis, ou retiré s'il n'existait pas).
# -Name : le journal est recopié dans build\smoke\<nom>.log, les captures dans build\smoke\<nom>\.
#
# Code de sortie : 0 = bon, 1 = problème (détail affiché).
param([int]$Seconds = 60, [switch]$Vr, [string]$Script, [switch]$ExpectStereo, [string]$Set, [string]$Name = "demarrage")

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\Release"
$log = Join-Path $bin "sr2_vr.log"
$shots = Join-Path $bin "captures"
$keep = Join-Path $root "build\smoke"
$exe = "SR2_pc"
Write-Output "--- essai '$Name' ($(Get-Date -Format HH:mm:ss)) ---"

if (Get-Process $exe -ErrorAction SilentlyContinue) {
    Write-Output "ECHEC : le jeu tourne deja, ferme-le avant l'essai"
    exit 1
}
if ($Vr) {
    & (Join-Path $PSScriptRoot "vr_null.ps1") start
    if ($LASTEXITCODE -ne 0) { exit 1 }
    $env:VR_CONFIG_PATH = Join-Path $root "build\vr_null\config"
    $env:VR_LOG_PATH = Join-Path $root "build\vr_null\logs"
}
# Réglages de l'essai : sr2_vr.ini à côté de la DLL, remis en état à la fin
$ini = Join-Path $bin "sr2_vr.ini"
$iniBackup = "$ini.avant_essai"
$iniExisted = Test-Path $ini
if ($Set) {
    if ($iniExisted) { Copy-Item $ini $iniBackup -Force }
    Add-Type -Namespace Sr2 -Name Ini -MemberDefinition '[DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool WritePrivateProfileString(string section, string key, string value, string path);'
    foreach ($pair in $Set.Split(",")) {
        if ($pair -match "^\s*(\w+)\.(\w+)=(.*)$") {
            [Sr2.Ini]::WritePrivateProfileString($Matches[1], $Matches[2], $Matches[3].Trim(), $ini) | Out-Null
        }
    }
    Write-Output "reglages de l'essai : $Set"
}
$start = Get-Date
$scriptCopy = Join-Path $bin "sr2_vr_test.txt"
if ($Script) { Copy-Item (Join-Path $root $Script) $scriptCopy -Force }
if (Test-Path $shots) { Get-ChildItem $shots -Filter "essai_*.bmp" | Remove-Item -Confirm:$false }

$arguments = @("--no-launcher")
if ($Vr) { $arguments += "--steamvr" }
$injector = Start-Process -FilePath (Join-Path $bin "sr2_vr_injector.exe") -ArgumentList $arguments `
    -WorkingDirectory $bin -PassThru -WindowStyle Hidden
$deadline = (Get-Date).AddSeconds($Seconds)
$alive = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if (Get-Process $exe -ErrorAction SilentlyContinue) { $alive = $true }
    elseif ($alive) { break } # le jeu s'est arrêté avant la fin : plantage
}
$running = Get-Process $exe -ErrorAction SilentlyContinue
$crashed = $alive -and -not $running
if ($running) {
    $running | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 3
    Get-Process $exe -ErrorAction SilentlyContinue | Stop-Process -Force
}
if (-not $injector.HasExited) { $injector | Stop-Process -Force }
if ($Vr) { & (Join-Path $PSScriptRoot "vr_null.ps1") stop | Out-Null }
if ($Script -and (Test-Path $scriptCopy)) { Remove-Item $scriptCopy -Confirm:$false }
if ($Set) {
    if ($iniExisted) { Move-Item $iniBackup $ini -Force }
    elseif (Test-Path $ini) { Remove-Item $ini -Confirm:$false }
}

# Journal et captures gardés pour être relus après l'essai suivant
New-Item -ItemType Directory -Force $keep | Out-Null
if (Test-Path $log) { Copy-Item $log (Join-Path $keep "$Name.log") -Force }
$kept = 0
if (Test-Path $shots) {
    $files = Get-ChildItem $shots -Filter "essai_*.bmp"
    if ($files) {
        New-Item -ItemType Directory -Force (Join-Path $keep $Name) | Out-Null
        $files | Copy-Item -Destination (Join-Path $keep $Name) -Force
        $kept = $files.Count
    }
}

$problems = @()
if (-not $alive) { $problems += "le jeu n'a pas demarre" }
if ($crashed) { $problems += "le jeu s'est arrete avant la fin de l'essai (plantage ?)" }
if (-not (Test-Path $log) -or (Get-Item $log).LastWriteTime -lt $start) {
    $problems += "pas de journal : la DLL n'a pas ete injectee"
} else {
    $lines = Get-Content $log -Encoding UTF8
    # Sans casque, le runtime refuse l'instance OpenXR : attendu, lignes [xr] ignorées
    $errors = $lines | Select-String -Pattern "ERREUR|ECHEC|introuvable|indisponible|ATTENTION|ALERTE" |
        Where-Object { $Vr -or $_ -notmatch "\[xr\]" } |
        Where-Object { $_ -notmatch "thread XR bloque dans xrCreateInstance|liste des modules indisponible" } # diagnostics sans effet
    foreach ($e in $errors) { $problems += "journal : $($e.Line.Trim())" }
    $version = $lines | Select-String -Pattern "DLL injectee \((.*)\)" | Select-Object -First 1
    $signatures = ($lines | Select-String -Pattern "signature .* trouvee").Count
    if ($signatures -lt 30) { $problems += "seulement $signatures signatures trouvees dans le jeu" }
    if (-not ($lines | Select-String -Pattern "camera : simulation detournee, rendu detourne")) {
        $problems += "les detours de la camera ne sont pas tous poses"
    }
    $frames = $lines | Select-String -Pattern "\[render\] images : (\d+) au total" | Select-Object -Last 1
    if (-not $frames) { $problems += "aucune image du jeu vue par le detour du Present" }
    if ($Vr) {
        if (-not ($lines | Select-String -Pattern "etat de session : FOCUSED")) {
            $problems += "la session OpenXR n'a pas atteint FOCUSED"
        }
        $xr = $lines | Select-String -Pattern "casque : ([\d.]+) images par seconde soumises, ([\d.]+) images du jeu par seconde"
        $best = ($xr | ForEach-Object { [double]::Parse($_.Matches[0].Groups[2].Value, [cultureinfo]::InvariantCulture) } |
            Measure-Object -Maximum).Maximum
        if (-not $xr) { $problems += "aucune image soumise au casque" }
        elseif ($best -lt 5) { $problems += "le jeu n'envoie pas d'images au casque : $($xr[-1].Line.Trim())" }
        else { Write-Output ("casque simule : " + ($xr[-1].Line -replace "^.*casque : ", "")) }
        if ($ExpectStereo) {
            $pairs = ($lines | Select-String -Pattern "paires du jeu : ([\d.]+) par" |
                ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, [cultureinfo]::InvariantCulture) } |
                Measure-Object -Maximum).Maximum
            if (-not $pairs -or $pairs -lt 5) { $problems += "pas de paires d'images : la partie n'a pas demarre ou le vrai relief ne fonctionne pas" }
            else {
                $all = @($lines | Select-String -Pattern "paires du jeu : ([\d.]+) par" |
                    ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, [cultureinfo]::InvariantCulture) } |
                    Where-Object { $_ -ge 5 } | Sort-Object)
                Write-Output ("vrai relief : {0} paires par seconde (mediane de {1} releves de 10 s, maximum {2})" -f
                    $all[[int][math]::Floor($all.Count / 2)], $all.Count, $pairs)
            }
            $timing = $lines | Select-String -Pattern "temps par image de simulation .* : (.*)$" | Select-Object -Last 1
            if ($timing) { Write-Output ("temps d'une paire : " + $timing.Matches[0].Groups[1].Value) }
            $total = $lines | Select-String -Pattern "ms pour l'oeil droit .* total ([\d.]+ ms)" | Select-Object -Last 1
            if ($total) { Write-Output ("  " + ($total.Line -replace "^.*\[engine\]\s*", "")) }
            $incomplete = ($lines | Select-String -Pattern "paires du jeu : [\d.]+ par seconde, (\d+) incomplete" |
                ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Sum).Sum
            if ($incomplete -gt 5) { $problems += "$incomplete paires incompletes (oeil droit manquant)" }
            # Les deux yeux d'une même capture : même paire, et de la lumière dans l'œil droit
            $right = $lines | Select-String -Pattern "capture (\d+), oeil droit : \d+x\d+, luminosite moyenne (\d+), image n. \d+, paire (\d+)"
            $lit = @($right | Where-Object { [int]$_.Matches[0].Groups[2].Value -ge 5 -and [int]$_.Matches[0].Groups[3].Value -gt 0 })
            if ($Script -and -not $lit.Count) { $problems += "aucune capture de l'oeil droit avec une image (oeil droit noir ?)" }
            foreach ($r in $lit) {
                $n = $r.Matches[0].Groups[1].Value
                $left = $lines | Select-String -Pattern "capture $n, oeil gauche : .* paire (\d+)" | Select-Object -First 1
                if ($left -and $left.Matches[0].Groups[1].Value -ne $r.Matches[0].Groups[3].Value) {
                    $problems += "capture $n : les deux yeux ne viennent pas de la meme paire"
                }
            }
        }
    }
    if ($Script) {
        if (-not ($lines | Select-String -Pattern "script d'entrees termine")) { $problems += "le script d'entrees n'est pas alle au bout" }
        $lines | Select-String -Pattern "\[test\] (pad|axis|key) .*(pas prete|non envoyee|inconnu)" |
            ForEach-Object { $problems += "script : $($_.Line.Trim())" }
    }
    Write-Output ("journal : {0}, {1} lignes, {2} signatures, {3}, {4} capture(s)" -f
        $(if ($version) { $version.Matches[0].Groups[1].Value } else { "version ?" }), $lines.Count, $signatures,
        $(if ($frames) { $frames.Line -replace "^.*\[render\] ", "" } else { "pas de compteur d'images" }), $kept)
}
if ($problems.Count) {
    Write-Output "ECHEC :"
    $problems | ForEach-Object { Write-Output "  $_" }
    exit 1
}
Write-Output "BON"
exit 0
