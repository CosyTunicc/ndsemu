# Prepare le dossier a donner a un ami : l'executable, le mode d'emploi et les licences, sans jeu, sauvegarde,
# reglages ni firmware (firmware.bin, *.ini, etats\ restent sur ton PC : ce sont les tiens).
# A lancer depuis le dossier du projet, apres avoir compile en Release :
#   powershell -ExecutionPolicy Bypass -File .\preparer_partage.ps1
# Publier une mise a jour sur GitHub (les ndsemu deja installes la proposeront) : augmenter la version dans
# CMakeLists.txt, recompiler, ecrire les nouveautes dans NOUVEAUTES.md, puis :
#   powershell -ExecutionPolicy Bypass -File .\preparer_partage.ps1 -Publier
param([switch]$Publier)
$racine = $PSScriptRoot
$exe = Join-Path $racine "build\Release\ndsemu.exe"
if (-not (Test-Path $exe)) {
    Write-Host "ndsemu.exe est introuvable ($exe)."
    Write-Host "Compile d'abord : cmake --build build --config Release"
    Write-Host "S'il vient d'etre compile puis a disparu, l'antivirus l'a probablement mis en quarantaine (voir plus bas)."
    exit 1
}
$dest = Join-Path $racine "ndsemu_a_partager"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Path $dest | Out-Null
Copy-Item $exe $dest
Copy-Item (Join-Path $racine "LISEZMOI.txt") $dest
Copy-Item (Join-Path $racine "LICENCES.txt") $dest          # licence de Dear ImGui (a fournir avec le programme)
Start-Sleep -Seconds 2
if (-not (Test-Path (Join-Path $dest "ndsemu.exe"))) {
    Write-Host "ATTENTION : ndsemu.exe a disparu du dossier $dest juste apres la copie."
    Write-Host "L'antivirus (Securite Windows) l'a probablement supprime. Voir : Securite Windows > Protection contre les virus > Historique de protection."
    exit 1
}
$zip = Join-Path $racine "ndsemu_a_partager.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dest "*") -DestinationPath $zip

# Verification : on relit le contenu de l'archive
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
$trouve = $false
Write-Host "Contenu de l'archive :"
foreach ($entree in $archive.Entries) {
    Write-Host ("  - " + $entree.FullName + " (" + [math]::Round($entree.Length / 1KB) + " Ko)")
    if ($entree.Name -eq "ndsemu.exe") { $trouve = $true }
}
$archive.Dispose()
if ($trouve) {
    Write-Host "Archive prete : $zip"
    Write-Host "Le jeu (.nds) n'est PAS inclus : ton ami doit utiliser sa propre copie."
} else {
    Write-Host "ERREUR : ndsemu.exe n'est pas dans l'archive."
    exit 1
}

# Archive pour GitHub Releases, nommee avec la version (lue dans CMakeLists.txt)
$cmake = Get-Content (Join-Path $racine "CMakeLists.txt") -Raw
if ($cmake -match 'project\(ndsemu VERSION ([0-9.]+)') { $version = $Matches[1] } else { $version = "0.0.0" }
$zipVersion = Join-Path $racine "ndsemu-windows-$version.zip"
Copy-Item $zip $zipVersion -Force
Write-Host "Archive de la version ${version} : $zipVersion"

if ($Publier) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        Write-Host "GitHub CLI (gh) est introuvable : installe-le (winget install GitHub.cli) puis connecte-toi (gh auth login)."
        Write-Host "Ou publie a la main : sur GitHub, Releases > Draft a new release, etiquette v$version, joins $zipVersion."
        exit 1
    }
    $notes = Join-Path $racine "NOUVEAUTES.md"
    if (-not (Test-Path $notes)) {
        Write-Host "Ecris d'abord les nouveautes de la version $version dans NOUVEAUTES.md (elles s'affichent dans ndsemu)."
        exit 1
    }
    gh release view "v$version" *> $null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "La version v$version existe deja sur GitHub : augmente la version dans CMakeLists.txt et recompile."
        exit 1
    }
    gh release create "v$version" "$zipVersion" --title "ndsemu $version" --notes-file "$notes"
    if ($LASTEXITCODE -ne 0) { Write-Host "La publication a echoue."; exit 1 }
    Write-Host "Version $version publiee : les ndsemu installes la proposeront au prochain demarrage."
}
