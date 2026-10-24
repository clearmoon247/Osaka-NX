# build-windows.ps1 - builds osaka-nx.nro on Windows by driving Ubuntu inside WSL2.
#
# Why this file exists
#     The ROM-to-C step of the build needs Linux tools, so on Windows the whole build runs inside WSL2 (Windows' built-in Linux
#     layer). This script is the glue: it makes sure WSL2 and Ubuntu 24.04 are present, copies this repository and your ROM into
#     Ubuntu's own file system (builds on the Windows drives are very slow), runs switch/build-nro.sh there, and copies the finished
#     osaka-nx.nro back to a normal Windows folder. All of the real build logic is in switch/build-nro.sh; read that file to audit it.
#
# How to start it
#     Double-click switch\build-windows.bat (which just runs this file), or from PowerShell:
#         powershell -ExecutionPolicy Bypass -File switch\build-windows.ps1 [-Rom C:\path\to\rom.z64] [-OutDir C:\some\folder]
#     Options:
#         -Rom        your Pokemon Stadium (USA) v1.0 ROM (.z64, .v64 or .n64). If left out, a file picker opens.
#         -OutDir     where the .nro is put (default: a folder called Osaka-NX on your Desktop)
#         -Distro     the WSL distribution to use (default: Ubuntu-24.04)
#         -Fast       quicker compile, larger and slower program (see build-nro.sh --fast)
#         -CheckOnly  only look at this computer and the ROM; change and build nothing
#
# What it never does: send anything over the network except the downloads that build-nro.sh makes (Ubuntu packages, devkitPro,
# GitHub sources), change your Windows settings, or leave anything outside the Ubuntu distribution except the output folder.
#
# This file is ASCII only on purpose: Windows PowerShell 5.1 reads files without a byte-order mark as ANSI.

param(
    [string]$Rom = "",
    [string]$OutDir = "",
    [string]$Distro = "Ubuntu-24.04",
    [switch]$Fast,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

# ---- Settings ---------------------------------------------------------------------------------------------------------------
$RepoWindows = Split-Path -Parent $PSScriptRoot          # the folder that holds switch\ (this repository)
$WorkDirName = "osaka-nx"                                # folder created in Ubuntu's home for the build copy
$RomDirName  = "osaka-nx-rom"                            # folder in Ubuntu's home that holds your ROM copy
if ($OutDir -eq "") { $OutDir = Join-Path ([Environment]::GetFolderPath("Desktop")) "Osaka-NX" }

# ---- Small helpers ----------------------------------------------------------------------------------------------------------
function Say($text)  { Write-Host $text }
function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Good($text) { Write-Host "  [ok]   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "  [warn] $text" -ForegroundColor Yellow }
function Bad($text)  { Write-Host "  [FIX]  $text" -ForegroundColor Red }

function Stop-Here($text) {
    Write-Host ""
    Write-Host $text -ForegroundColor Red
    exit 1
}

function Ask-Yes($question) {
    $reply = Read-Host "  $question [y/N]"
    return ($reply -match '^(y|yes)$')
}

# Runs wsl.exe and returns its output lines. wsl.exe writes its own messages (not Linux program output) as UTF-16, so the
# console's output encoding is switched for this one call and put back afterwards.
function Wsl-Text([string[]]$wslArgs) {
    $previous = [Console]::OutputEncoding
    [Console]::OutputEncoding = [System.Text.Encoding]::Unicode
    try { $lines = & wsl.exe @wslArgs 2>&1 } finally { [Console]::OutputEncoding = $previous }
    return @($lines | ForEach-Object { "$_".Replace([string][char]0, "").Trim() } | Where-Object { $_ -ne "" })
}

# Runs a bash command inside the chosen distribution, showing its output live. Returns the command's exit status.
# Windows PowerShell 5.1 mangles double quotes inside arguments given to programs, so the command is not passed directly: it is
# sent as base64 text (letters and digits only), written to a file inside Ubuntu, and then run from that file. Running it from a
# file (rather than piping it in) also keeps the keyboard connected, so sudo can ask for your Ubuntu password.
function Linux([string]$command) {
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($command.Replace("`r", "")))
    & wsl.exe -d $Distro --cd "~" -- bash -c "echo $encoded | base64 -d > /tmp/osaka-nx-step.sh"
    if ($LASTEXITCODE -ne 0) { return $LASTEXITCODE }
    & wsl.exe -d $Distro --cd "~" -- bash /tmp/osaka-nx-step.sh
    return $LASTEXITCODE
}

# Runs a bash command inside the distribution and returns what it printed (one trimmed string).
function Linux-Output([string]$command) {
    $out = & wsl.exe -d $Distro --cd "~" -- bash -c $command 2>$null
    return ("$out").Trim()
}

# Converts a Windows path to the path Ubuntu sees it under (C:\Users\me -> /mnt/c/Users/me).
function To-WslPath([string]$windowsPath) {
    return Linux-Output ("wslpath -a '" + $windowsPath.Replace("'", "'\''") + "'")
}

# Quotes a text for use inside a bash command line.
function Q([string]$text) { return "'" + $text.Replace("'", "'\''") + "'" }

# ---- 1. Windows side checks -------------------------------------------------------------------------------------------------
Step "Checking this computer"

if (-not (Test-Path (Join-Path $RepoWindows "switch\build-nro.sh"))) {
    Stop-Here "Cannot find switch\build-nro.sh next to this script. Run this from inside the Osaka-NX folder."
}
if (-not (Test-Path (Join-Path $RepoWindows ".git"))) {
    Bad "This folder is not a git clone (a ZIP download will not work: the build needs git to fetch its submodules)."
    Stop-Here "Install Git for Windows, then run:  git clone --recursive <repository address>   and start this script from that folder."
}
Good "repository: $RepoWindows"

$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
if ($ram -ge 12) { Good "memory: $ram GB" }
else { Warn "memory: $ram GB. WSL gets half by default, which is tight. Put the two lines [wsl2] and memory=8GB in %UserProfile%\.wslconfig, then run  wsl --shutdown  and start this script again." }

$cFree = [math]::Round((Get-PSDrive -Name C -ErrorAction SilentlyContinue).Free / 1GB)
if ($cFree -ge 25) { Good "free space on C: $cFree GB" }
else { Warn "free space on C: $cFree GB (the Ubuntu disk image grows to about 20 GB during the build)" }

# ---- 2. WSL2 and Ubuntu -----------------------------------------------------------------------------------------------------
Step "Checking WSL2 and $Distro"

$haveWsl = [bool](Get-Command wsl.exe -ErrorAction SilentlyContinue)
$distros = @()
if ($haveWsl) { $distros = Wsl-Text @("-l", "-q") }
$haveDistro = $distros -contains $Distro

if (-not $haveDistro) {
    Bad "$Distro is not installed in WSL."
    if ($CheckOnly) { Stop-Here "Check finished: WSL2 with $Distro still has to be installed (run this script without -CheckOnly)." }
    Say "  Installing it is done by Windows itself:  wsl --install -d $Distro"
    Say "  It needs administrator rights and may ask you to restart the computer."
    if (-not (Ask-Yes "Install WSL2 and $Distro now?")) { Stop-Here "Stopped: install WSL2 and $Distro, then run this script again." }
    Start-Process -FilePath "wsl.exe" -ArgumentList @("--install", "-d", $Distro) -Verb RunAs -Wait
    Say ""
    Say "Next steps (Windows cannot do these for the script):"
    Say "  1. If Windows asks you to restart, restart."
    Say "  2. Open 'Ubuntu 24.04' from the Start menu once and choose a user name and password when it asks."
    Say "  3. Run build-windows.bat again."
    exit 0
}
Good "$Distro is installed"

# The build needs WSL version 2 (full Linux kernel, fast file system).
$listing = Wsl-Text @("-l", "-v")
$mine = $listing | Where-Object { $_ -match [regex]::Escape($Distro) } | Select-Object -First 1
if ($mine -and $mine -match '\s1$') {
    Bad "$Distro runs as WSL version 1."
    if ($CheckOnly -or -not (Ask-Yes "Convert it to WSL 2 now (takes a few minutes)?")) { Stop-Here "Convert it with:  wsl --set-version $Distro 2   then run this script again." }
    & wsl.exe --set-version $Distro 2
    if ($LASTEXITCODE -ne 0) { Stop-Here "The conversion failed. See the message above." }
}
else { Good "WSL version 2" }

$who = Linux-Output "id -un"
if ($who -eq "") { Stop-Here "Could not start $Distro. Open 'Ubuntu 24.04' from the Start menu once to finish its first-time setup, then run this script again." }
if ($who -eq "root") { Warn "the distribution's default user is root; that works, but a normal user is safer" }
else { Good "Ubuntu user: $who" }

# ---- 3. The ROM -------------------------------------------------------------------------------------------------------------
Step "Your ROM"

if ($Rom -eq "") {
    Say "  Choose your Pokemon Stadium (USA) v1.0 ROM file (.z64, .v64 or .n64). The file is only read and copied into Ubuntu."
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = "Choose your Pokemon Stadium (USA) v1.0 ROM"
        $dialog.Filter = "N64 ROM (*.z64;*.v64;*.n64;*.rom;*.bin)|*.z64;*.v64;*.n64;*.rom;*.bin|All files (*.*)|*.*"
        $dialog.InitialDirectory = [Environment]::GetFolderPath("UserProfile")
        if ($dialog.ShowDialog() -eq "OK") { $Rom = $dialog.FileName }
    } catch { Warn "the file picker could not open: $($_.Exception.Message)" }
    if ($Rom -eq "") { $Rom = (Read-Host "  Type or paste the full path of the ROM file").Trim('"') }
}
$Rom = $Rom.Trim('"')
if (-not (Test-Path -LiteralPath $Rom -PathType Leaf)) { Stop-Here "File not found: $Rom" }
Good "ROM file: $Rom"

# ---- 4. Copy the repository and the ROM into Ubuntu ---------------------------------------------------------------------------
Step "Copying files into Ubuntu (this is a one-time copy; your Windows files are not changed)"

$repoWsl = To-WslPath $RepoWindows
$romWsl  = To-WslPath $Rom
if ($repoWsl -eq "" -or $romWsl -eq "") { Stop-Here "Ubuntu could not see those Windows paths (wslpath failed)." }

# tar copies the tree and leaves out build products, so running the script again keeps earlier progress in Ubuntu (downloaded
# sources, generated code) instead of starting from nothing. Afterwards:
#   - core.fileMode is switched off and scripts are made executable, because files read from a Windows drive look "all executable";
#   - carriage returns are removed from text files in case a Windows checkout added them (they would break shell scripts and patches).
$copyScript = @"
set -e
mkdir -p ~/$WorkDirName ~/$RomDirName
tar -C $(Q $repoWsl) \
    --exclude=./generated --exclude=./lib/N64ModernRuntime --exclude=./lib/GLideN64 --exclude=./lib/N64Recomp \
    --exclude=./switch/build --exclude=./.osaka-work --exclude=./.venv-codegen --exclude=./disasm/build \
    -cf - . | tar -C ~/$WorkDirName -xf -
cp -f $(Q $romWsl) ~/$RomDirName/
cd ~/$WorkDirName
git config core.fileMode false
git config --global --add safe.directory "`$PWD" >/dev/null 2>&1 || true
find switch tools -type f \( -name '*.sh' -o -name '*.py' -o -name '*.patch' -o -name '*.pin' -o -name '*.toml' -o -name '*.cmake' -o -name '*.txt' \) \
    -exec sed -i 's/\r`$//' {} + 2>/dev/null || true
chmod +x switch/*.sh switch/tools/*.py 2>/dev/null || true
"@
$copyScript = $copyScript -replace "`r", ""
if ((Linux $copyScript) -ne 0) { Stop-Here "Copying into Ubuntu failed. See the message above." }
Good "repository copied to ~/$WorkDirName and ROM to ~/$RomDirName inside $Distro"

$romName = Split-Path -Leaf $Rom

# ---- 5. Build -----------------------------------------------------------------------------------------------------------------
$optionText = ""
if ($Fast) { $optionText += " --fast" }

$romArg = "`"`$HOME/$RomDirName/$romName`""
$outWsl = To-WslPath $OutDir.TrimEnd('\')
if ($outWsl -eq "") {
    # The folder does not exist yet, so ask for its parent and add the last part.
    $parent = Split-Path -Parent $OutDir
    $outWsl = (To-WslPath $parent) + "/" + (Split-Path -Leaf $OutDir)
}

if ($CheckOnly) {
    Step "Running the build script's own check (nothing is installed or built)"
    $code = Linux ("cd ~/$WorkDirName && bash switch/build-nro.sh $romArg --check")
    exit $code
}

Step "Building (first run: roughly 1 to 2 hours, mostly compiling; Ubuntu may ask for your Ubuntu password to install tools)"
Say "  Leave this window open. If it stops with a problem, fix it and run build-windows.bat again: finished steps are skipped."
$buildCommand = "cd ~/$WorkDirName && bash switch/build-nro.sh $romArg --install$optionText --out $(Q $outWsl)"
$code = Linux $buildCommand

# ---- 6. Result ----------------------------------------------------------------------------------------------------------------
Step "Result"
if ($code -eq 0 -and (Test-Path (Join-Path $OutDir "osaka-nx.nro"))) {
    Good "Built: $(Join-Path $OutDir 'osaka-nx.nro')"
    Say ""
    Say "  Put it on the Switch's SD card as  switch/osaka-nx.nro , make a folder  switch/osaka-nx/  next to it and put your ROM in that folder."
    Say "  Do not share the .nro: it contains code made from your game."
    Start-Process explorer.exe -ArgumentList @("/select,", (Join-Path $OutDir "osaka-nx.nro"))
    exit 0
}

# Failure: bring the report that build-nro.sh wrote (system facts and the end of the failing log, no ROM data) to a visible place.
Bad "The build did not finish (exit status $code)."
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$copyReport = "if [ -f ~/$WorkDirName/osaka-nx-build-report.txt ]; then cp -f ~/$WorkDirName/osaka-nx-build-report.txt $(Q $outWsl)/; fi"
Linux $copyReport | Out-Null
if (Test-Path (Join-Path $OutDir "osaka-nx-build-report.txt")) {
    Say "  A report was saved to: $(Join-Path $OutDir 'osaka-nx-build-report.txt')"
    Say "  It lists this computer's facts and the end of the log where it failed; it contains no ROM data. Attach it when asking for help."
}
exit 1
