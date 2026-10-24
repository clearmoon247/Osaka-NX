# build-windows-native.ps1 - EXPERIMENTAL: builds osaka-nx.nro on Windows WITHOUT WSL, Docker or any Linux system.
#
# Status
#     Written without access to a Windows machine: none of this has been run yet. The pieces were chosen because each one is
#     published for Windows by its own project, but expect to need a fix or two on the first run. When something fails, the
#     script saves a report (see "If it fails" below) that shows exactly where. The WSL launcher, switch\build-windows.bat,
#     is the tested-by-design alternative.
#
# Why this is hard
#     Turning your ROM into C code first rebuilds the game from the pret/pokestadium decompilation. That project compiles with
#     SGI's IDO compiler and GNU MIPS binutils, normally Linux programs, and its own Makefile says "native Windows is
#     unsupported". This script assembles the Windows versions of those tools and runs the same Makefile inside MSYS2, a
#     small Unix-style shell for Windows. pret's build then compares the ROM it rebuilt with yours byte for byte, so if the
#     Windows tools differed from the Linux ones in any way that matters, the build stops there instead of producing a wrong game.
#
# What it installs (all under one folder, default C:\OsakaNX, no administrator rights needed; delete the folder to remove it)
#     msys2        Unix-style shell, make, git, gcc and, later, a MinGW-w64 compiler for the N64Recomp tool
#                  from https://github.com/msys2/msys2-installer (checked against a SHA-256 written in this file)
#     python       Python 3.12 for Windows from python.org (checked: must carry the Python Software Foundation signature)
#     mips         MIPS binutils for Windows, https://github.com/n64-tools/gcc-toolchain-mips64 (SHA-256 checked)
#     downloads    the IDO compiler for Windows, https://github.com/decompals/ido-static-recomp release v1.2 (SHA-256 checked)
#     osaka-nx     a working copy of this repository (your own folder is not modified)
#     logs         one log file per stage
#     Nothing is installed system-wide and no Windows setting is changed.
#
# What it needs from you
#     * about 30 GB free disk space, a 64-bit Windows 10 or 11, an internet connection, Git for Windows to have cloned this repo
#     * your Pokemon Stadium (USA) v1.0 ROM (a file picker opens)
#     * for the last step (compiling for the Switch) devkitPro with "Switch development", from https://devkitpro.org
#       If devkitPro is missing, the script stops after "generated/" is made and tells you what to install; run it again after.
#
# How to start it
#     Double-click switch\build-windows-native.bat, or from PowerShell:
#         powershell -ExecutionPolicy Bypass -File switch\build-windows-native.ps1 [-Rom C:\path\rom.z64] [-OutDir C:\folder]
#     Options: -Rom, -OutDir (default Desktop\Osaka-NX), -Root (tools folder, default C:\OsakaNX, no spaces allowed),
#              -Fast (quicker, larger program), -CheckOnly (look and report, install nothing),
#              -SkipHashCheck (only if a download was republished and you have checked it yourself)
#
# If it fails
#     The script writes osaka-nx-native-report.txt into the output folder: Windows version, memory, disk, the stage that
#     failed and the end of its log. It contains no ROM data and your user name is replaced. Re-running continues where it
#     stopped (finished downloads and finished build stages are skipped).
#
# This file is ASCII only on purpose: Windows PowerShell 5.1 reads files without a byte-order mark as ANSI.

param(
    [string]$Rom = "",
    [string]$OutDir = "",
    [string]$Root = "",
    [switch]$Fast,
    [switch]$CheckOnly,
    [switch]$SkipHashCheck
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"          # Invoke-WebRequest is very slow with its progress bar
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }

# ---- What gets downloaded (every file except Python is pinned by SHA-256) -----------------------------------------------------
$Downloads = @{
    Msys2 = @{
        Url    = "https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-base-x86_64-20260927.sfx.exe"
        Sha256 = "ad336cccfda47758b5e15cda993fbba421115cb0b126697daef1ee4dfe37209f"
        File   = "msys2-base-x86_64-20260927.sfx.exe"
    }
    Ido71 = @{
        Url    = "https://github.com/decompals/ido-static-recomp/releases/download/v1.2/ido-7.1-recomp-windows.tar.gz"
        Sha256 = "28487a90a47d2565d8c38235750500f6fbd3479b758e966b529166825804c926"
        File   = "ido-7.1-recomp-windows.tar.gz"
    }
    Ido53 = @{
        Url    = "https://github.com/decompals/ido-static-recomp/releases/download/v1.2/ido-5.3-recomp-windows.tar.gz"
        Sha256 = "2268f64cab973750e78bdf7aa2ff3cccf78ad1d181507da6faeb61f3d22c2e1e"
        File   = "ido-5.3-recomp-windows.tar.gz"
    }
    Mips = @{
        # "latest" is a rolling tag. If the project publishes a new build this check fails on purpose; see -SkipHashCheck.
        Url    = "https://github.com/n64-tools/gcc-toolchain-mips64/releases/download/latest/gcc-toolchain-mips64-win64.zip"
        Sha256 = "7ee3598ac151c0a728dcfd916e3df615793d2ed0a28cdc0ccafa31eef76526bb"
        File   = "gcc-toolchain-mips64-win64.zip"
    }
    Python = @{
        Url    = "https://www.python.org/ftp/python/3.12.8/python-3.12.8-amd64.exe"
        Sha256 = ""                                   # not pinned; the installer's digital signature is checked instead
        File   = "python-3.12.8-amd64.exe"
    }
}
$PacmanPackagesMsys  = "make git gcc libiconv diffutils patch findutils tar unzip zip gawk sed grep file"
$PacmanPackagesMingw = "mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja"

# ---- Paths --------------------------------------------------------------------------------------------------------------------
$RepoWindows = Split-Path -Parent $PSScriptRoot                     # the folder that holds switch\ (this repository)
if ($Root -eq "") { $Root = Join-Path $env:SystemDrive "OsakaNX" }
if ($OutDir -eq "") { $OutDir = Join-Path ([Environment]::GetFolderPath("Desktop")) "Osaka-NX" }
$Dl      = Join-Path $Root "downloads"
$Logs    = Join-Path $Root "logs"
$Tmp     = Join-Path $Root "tmp"
$Work    = Join-Path $Root "osaka-nx"
$Msys    = Join-Path $Root "msys64"
$Bash    = Join-Path $Msys "usr\bin\bash.exe"
$PyDir   = Join-Path $Root "python"
$Py      = Join-Path $PyDir "python.exe"
$MipsDir = Join-Path $Root "mips"
$RomOut  = Join-Path $Root "rom\baserom.z64"

# ---- Small helpers ------------------------------------------------------------------------------------------------------------
function Say($text)  { Write-Host $text }
function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Good($text) { Write-Host "  [ok]   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "  [warn] $text" -ForegroundColor Yellow }
function Bad($text)  { Write-Host "  [FIX]  $text" -ForegroundColor Red }

$script:CurrentStage = "starting"
$script:LastLog = ""

# Converts C:\OsakaNX\foo to /c/OsakaNX/foo, the way MSYS2 writes Windows paths.
function To-Msys([string]$windowsPath) {
    $p = $windowsPath.Replace("\", "/")
    if ($p -match '^([A-Za-z]):(.*)$') { return "/" + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

# Writes the report next to the output and stops the script.
function Fail([string]$message) {
    Write-Host ""
    Write-Host $message -ForegroundColor Red
    try {
        New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
        $report = Join-Path $OutDir "osaka-nx-native-report.txt"
        $os = (Get-CimInstance Win32_OperatingSystem)
        $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
        $drive = (Get-PSDrive -Name ($Root.Substring(0, 1)) -ErrorAction SilentlyContinue)
        $lines = @(
            "Osaka-NX native Windows build report",
            "when:    $(Get-Date -Format s)",
            "windows: $($os.Caption) $($os.Version) ($($os.OSArchitecture))",
            "memory:  $ram GB   cpus: $([Environment]::ProcessorCount)",
            "free:    $([math]::Round($drive.Free / 1GB)) GB on $($Root.Substring(0, 2))",
            "stage:   $script:CurrentStage",
            "message: $message",
            ""
        )
        if ($script:LastLog -ne "" -and (Test-Path $script:LastLog)) {
            $lines += "---- end of $(Split-Path -Leaf $script:LastLog) ----"
            $lines += (Get-Content -LiteralPath $script:LastLog -Tail 80)
        }
        $text = ($lines -join "`r`n")
        $text = $text.Replace($env:USERNAME, "<user>")
        [IO.File]::WriteAllText($report, $text, (New-Object Text.UTF8Encoding($false)))
        Write-Host "A report was saved to: $report" -ForegroundColor Yellow
        Write-Host "It has no ROM data and your user name is replaced. Attach it when asking for help." -ForegroundColor Yellow
    } catch { }
    exit 1
}

function Ask-Yes($question) {
    $reply = Read-Host "  $question [y/N]"
    return ($reply -match '^(y|yes)$')
}

# Downloads a file once (kept in $Dl) and checks its SHA-256.
function Get-Verified($entry) {
    $dest = Join-Path $Dl $entry.File
    $check = ($entry.Sha256 -ne "") -and (-not $SkipHashCheck)
    if (Test-Path $dest) {
        if (-not $check -or ((Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToLower() -eq $entry.Sha256)) { return $dest }
        Remove-Item -Force $dest
    }
    Say "  downloading $($entry.File)"
    New-Item -ItemType Directory -Force -Path $Dl | Out-Null
    try { Invoke-WebRequest -UseBasicParsing -Uri $entry.Url -OutFile $dest }
    catch { Fail "Download failed: $($entry.Url) ($($_.Exception.Message)). Check the internet connection and run again." }
    if ($check) {
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToLower()
        if ($actual -ne $entry.Sha256) {
            Remove-Item -Force $dest
            Fail ("The downloaded file does not match the checksum written in this script:`r`n  file:     $($entry.Url)`r`n" +
                  "  expected: $($entry.Sha256)`r`n  got:      $actual`r`n" +
                  "It may have been republished. If you have looked at it and trust it, run again with -SkipHashCheck.")
        }
    }
    Unblock-File -LiteralPath $dest -ErrorAction SilentlyContinue
    return $dest
}

# Runs a bash script inside MSYS2, shows its output live and appends it to a log file. Returns the script's exit status.
# The script text is written to a file first (and run from there): passing multi-line text on the command line would
# lose its quotes in Windows PowerShell 5.1.
function Invoke-Msys([string]$name, [string]$body) {
    New-Item -ItemType Directory -Force -Path $Tmp, $Logs | Out-Null
    $log = Join-Path $Logs "$name.log"
    $script:LastLog = $log
    $jobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, [int][math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 3GB)))
    $preamble = @"
set -euo pipefail
export PSR_JOBS='$jobs'
export OSAKA_PY='$(To-Msys $Py)'
export OSAKA_MIPS_BIN='$(To-Msys (Join-Path $MipsDir "bin"))'
export OSAKA_IDO71='$(To-Msys (Join-Path $Dl $Downloads.Ido71.File))'
export OSAKA_IDO53='$(To-Msys (Join-Path $Dl $Downloads.Ido53.File))'
git config --global core.autocrlf false
git config --global --add safe.directory '*' || true
"@
    $text = ($preamble + "`n" + $body + "`n") -replace "`r", ""
    $scriptFile = Join-Path $Tmp "$name.sh"
    [IO.File]::WriteAllText($scriptFile, $text, (New-Object Text.UTF8Encoding($false)))

    $env:MSYSTEM = "MSYS"; $env:CHERE_INVOKING = "1"; $env:MSYS2_PATH_TYPE = "minimal"; $env:LANG = "C.UTF-8"
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    Set-Content -LiteralPath $log -Value "# $name  $(Get-Date -Format s)" -Encoding UTF8
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"          # PowerShell 5.1 turns a native program's stderr into errors otherwise
    try {
        & $Bash -l (To-Msys $scriptFile) 2>&1 | ForEach-Object {
            $line = "$_"
            Write-Host $line
            Add-Content -LiteralPath $log -Value $line -Encoding UTF8
        }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    return $code
}

# ---- 1. Windows checks --------------------------------------------------------------------------------------------------------
Step "Checking this computer"
$script:CurrentStage = "checking the computer"

if (-not (Test-Path (Join-Path $RepoWindows "switch\codegen-msys.sh"))) { Fail "Cannot find switch\codegen-msys.sh next to this script. Run this from inside the Osaka-NX folder." }
if (-not (Test-Path (Join-Path $RepoWindows ".git"))) { Fail "This folder is not a git clone (a ZIP download will not work: the build fetches submodules with git). Run:  git clone --recursive <repository address>" }
if (-not [Environment]::Is64BitOperatingSystem) { Fail "A 64-bit Windows is required." }
Good "repository: $RepoWindows"

if ($Root -match '[^\x20-\x7E]' -or $Root -match '\s') { Fail "The tools folder ($Root) must not contain spaces or non-ASCII characters (make cannot handle them). Choose another with -Root, for example C:\OsakaNX." }
$driveLetter = $Root.Substring(0, 1)
$free = [math]::Round((Get-PSDrive -Name $driveLetter).Free / 1GB)
if ($free -ge 30) { Good "free space on ${driveLetter}: $free GB" } else { Warn "free space on ${driveLetter}: $free GB (about 30 GB are needed)" }
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
if ($ram -ge 8) { Good "memory: $ram GB" } else { Warn "memory: $ram GB (8 GB or more is recommended; the Switch compile uses a lot)" }

$DevkitPro = ""
foreach ($candidate in @($env:DEVKITPRO, "C:\devkitPro")) {
    if ($candidate -and (Test-Path (Join-Path $candidate "devkitA64"))) { $DevkitPro = $candidate; break }
}
if ($DevkitPro -ne "") { Good "devkitPro found: $DevkitPro" }
else { Warn "devkitPro is not installed. The script can still make generated/ (the long part); install devkitPro (https://devkitpro.org/wiki/Getting_Started, tick 'Switch development') before the final compile." }

if ($CheckOnly) {
    Say ""
    Say "Check finished: nothing was installed. A full run would download about 400 MB and install Python, MSYS2 and the tools under $Root."
    exit 0
}

# ---- 2. Python and the ROM (small downloads first, so a wrong ROM is noticed early) ------------------------------------------
Step "Python for Windows"
$script:CurrentStage = "installing Python"
if (-not (Test-Path $Py)) {
    $installer = Get-Verified $Downloads.Python
    $sig = Get-AuthenticodeSignature -LiteralPath $installer
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Python Software Foundation") {
        Remove-Item -Force $installer
        Fail "The Python installer does not carry the Python Software Foundation signature, so it was deleted and not run."
    }
    Say "  installing Python 3.12 into $PyDir (per user, no administrator rights needed)"
    $installArgs = @("/quiet", "InstallAllUsers=0", "TargetDir=$PyDir", "PrependPath=0", "Include_launcher=0", "Include_test=0",
              "Include_doc=0", "Include_tcltk=0", "Include_pip=1", "Shortcuts=0", "AssociateFiles=0")
    $p = Start-Process -FilePath $installer -ArgumentList $installArgs -Wait -PassThru
    if ($p.ExitCode -ne 0 -or -not (Test-Path $Py)) { Fail "The Python installer failed (exit status $($p.ExitCode))." }
}
Good "Python: $(& $Py --version 2>&1)"

Step "Your ROM"
$script:CurrentStage = "checking the ROM"
if ($Rom -eq "") {
    Say "  Choose your Pokemon Stadium (USA) v1.0 ROM file (.z64, .v64 or .n64). The file is only read."
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
if (-not (Test-Path -LiteralPath $Rom -PathType Leaf)) { Fail "File not found: $Rom" }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $RomOut) | Out-Null
$romTool = Join-Path $RepoWindows "switch\tools\rom_tool.py"
$out = & $Py -I $romTool convert $Rom $RomOut 2>&1
Say "  $out"
if ($LASTEXITCODE -ne 0) { Fail "That file is not the right game (see the message above). A z64 copy was not made." }
Good "ROM is Pokemon Stadium (USA) v1.0; a z64 copy is at $RomOut"

# ---- 3. MSYS2, MIPS binutils, IDO ---------------------------------------------------------------------------------------------
Step "MSYS2 (Unix-style tools for Windows)"
$script:CurrentStage = "installing MSYS2"
if (-not (Test-Path $Bash)) {
    $sfx = Get-Verified $Downloads.Msys2
    Say "  unpacking MSYS2 into $Msys (about 1 minute)"
    & $sfx -y "-o$Root" | Out-Null
    if (-not (Test-Path $Bash)) { Fail "MSYS2 did not unpack to $Msys." }
}
if (-not (Test-Path (Join-Path $Root "state\msys-ready"))) {
    Say "  first start and package update (several minutes; MSYS2 may restart its shell once, that is normal)"
    $code = Invoke-Msys "msys-update" "pacman --noconfirm --disable-download-timeout -Syuu || true"
    $code = Invoke-Msys "msys-update2" "pacman --noconfirm --disable-download-timeout -Syuu"
    if ($code -ne 0) { Fail "Updating MSYS2 failed (exit status $code)." }
    $code = Invoke-Msys "msys-packages" "pacman --noconfirm --needed --disable-download-timeout -S $PacmanPackagesMsys $PacmanPackagesMingw"
    if ($code -ne 0) { Fail "Installing the MSYS2 packages failed (exit status $code)." }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "state") | Out-Null
    Set-Content -Path (Join-Path $Root "state\msys-ready") -Value (Get-Date -Format s)
}
Good "MSYS2 is ready"

Step "MIPS binutils and the IDO compiler for Windows"
$script:CurrentStage = "downloading the compilers"
if (-not (Test-Path (Join-Path $MipsDir "bin\mips64-elf-ld.exe"))) {
    $zip = Get-Verified $Downloads.Mips
    Say "  unpacking into $MipsDir"
    Expand-Archive -LiteralPath $zip -DestinationPath $MipsDir -Force
    if (-not (Test-Path (Join-Path $MipsDir "bin\mips64-elf-ld.exe"))) { Fail "The MIPS binutils archive did not contain bin\mips64-elf-ld.exe." }
}
Good "MIPS binutils"
$null = Get-Verified $Downloads.Ido71
$null = Get-Verified $Downloads.Ido53
Good "IDO 7.1 and 5.3 archives (unpacked into the build by the next stage)"

# ---- 4. Working copy of the repository -----------------------------------------------------------------------------------------
Step "Copying the repository to $Work"
$script:CurrentStage = "copying the repository"
# Build products are left out, so running the script again keeps earlier progress in the copy (nothing there is deleted).
$skip = @("generated", "switch\build", "lib\N64ModernRuntime", "lib\GLideN64", "disasm\build", "disasm\.venv", ".osaka-work", ".venv-codegen") |
        ForEach-Object { Join-Path $RepoWindows $_ }
& robocopy.exe $RepoWindows $Work /E /XD @skip /XF *.nro *.elf /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "Copying the repository failed (robocopy status $LASTEXITCODE)." }
Good "repository copied"

$workMsys = To-Msys $Work
$romMsys  = To-Msys $RomOut
$fixCrlf  = "cd '$workMsys'`nfind switch -type f \( -name '*.sh' -o -name '*.py' -o -name '*.patch' -o -name '*.pin' \) -exec sed -i 's/\r$//' {} +`nchmod +x switch/*.sh || true"

# ---- 5. Stages ------------------------------------------------------------------------------------------------------------------
Step "Stage 1: fetching and patching third-party sources"
$script:CurrentStage = "stage 1 (setup_deps)"
$code = Invoke-Msys "stage1-setup-deps" "$fixCrlf`nbash switch/setup_deps.sh"
if ($code -ne 0) { Fail "Stage 1 failed (exit status $code). Log: $script:LastLog" }
Good "sources ready"

Step "Stage 2: turning your ROM into C code (the long part: roughly 1 to 2 hours)"
$script:CurrentStage = "stage 2 (codegen-msys)"
Say "  Leave this window open. If it stops, fix the cause and run the script again: finished steps are skipped."
$code = Invoke-Msys "stage2-codegen" "cd '$workMsys'`nbash switch/codegen-msys.sh '$romMsys'"
if ($code -ne 0) { Fail "Stage 2 failed (exit status $code). Log: $script:LastLog" }
Good "generated/ is ready (stays on this computer; never share it)"

if ($DevkitPro -eq "") {
    Step "Stopping before the Switch compile"
    Say "  Install devkitPro with 'Switch development' (https://devkitpro.org/wiki/Getting_Started), then run this script again."
    Say "  The work done so far is kept, so the next run goes straight to the compile."
    exit 0
}

Step "Stage 3: compiling for the Switch (roughly 30 to 90 minutes)"
$script:CurrentStage = "stage 3 (build)"
# MSYS2 reads C:\devkitPro as /opt/devkitpro (the path devkitPro's own scripts expect).
$fstab = Join-Path $Msys "etc\fstab"
$mapping = "$($DevkitPro.Replace('\', '/')) /opt/devkitpro none binary 0 0"
if (-not (Select-String -Path $fstab -SimpleMatch "/opt/devkitpro" -Quiet)) { Add-Content -Path $fstab -Value ("`n" + $mapping) }
$smallCode = if ($Fast) { "" } else { "export PSR_SMALL_CODE=OS" }
$build = @"
cd '$workMsys'
pacman --noconfirm --needed -S cmake ninja
export DEVKITPRO=/opt/devkitpro
export PATH=`$DEVKITPRO/devkitA64/bin:`$DEVKITPRO/tools/bin:`$PATH
$smallCode
bash switch/build.sh
"@
$code = Invoke-Msys "stage3-build" $build
if ($code -ne 0) { Fail "Stage 3 failed (exit status $code). Log: $script:LastLog" }

# ---- 6. Result ----------------------------------------------------------------------------------------------------------------
Step "Result"
$nro = Join-Path $Work "switch\build\osaka-nx.nro"
if (-not (Test-Path $nro)) { Fail "The build finished but $nro does not exist." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item -Force $nro $OutDir
Good "Built: $(Join-Path $OutDir 'osaka-nx.nro')"
Say ""
Say "  Put it on the Switch's SD card as  switch/osaka-nx.nro , make a folder  switch/osaka-nx/  next to it and put your ROM in that folder."
Say "  Do not share the .nro: it contains code made from your game."
Start-Process explorer.exe -ArgumentList @("/select,", (Join-Path $OutDir "osaka-nx.nro"))
exit 0
