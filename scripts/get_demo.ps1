<#
.SYNOPSIS
    Fetch the Nov 1997 Hexen II demo data so Hexenwail has something to run.

.DESCRIPTION
    Hexenwail is a game engine; it ships no game content.  Raven's data is not
    ours to hand out -- the only licence distributed with the demo is
    Activision's retail agreement, which does not grant redistribution (see
    assets/demo/README.md).  So this does not mirror anything: it downloads the
    package from the uHexen2 project, where it has been publicly hosted for two
    decades, under whatever terms apply to it there, and verifies that what
    arrived is what was expected.

    The demo is three levels of the Blackmarsh hub.  If you own Hexen II, on
    Steam, GOG or disc, you do not need this -- copy that installation's
    "data1" directory next to the Hexenwail executable instead.

.PARAMETER Destination
    Where to create "data1".  Defaults to the current directory.  Point it at
    the folder holding hexenwail.exe -- the engine prints that path as
    "basedir is: ..." on startup.

.EXAMPLE
    .\get_demo.ps1
    .\get_demo.ps1 "C:\Games\Hexenwail"

.NOTES
    Needs Windows 10 1803 or newer for the bundled tar.exe.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Destination = '.'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# downloads.sourceforge.net, not the /projects/.../download page: the latter
# answers with a 143 KB "your download will start shortly" HTML interstitial
# rather than the file.
$Url     = 'https://downloads.sourceforge.net/project/uhexen2/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz'
# SourceForge decides between the file and the interstitial by User-Agent, and
# Invoke-WebRequest's default identifies as a browser -- which gets the HTML on
# the old URL and a flat 403 on this one.  Anything tool-shaped is served the
# file.  Do not "tidy" this away; without it the download silently returns a
# web page and only the checksum below catches it.
$UserAgent = 'curl/8.0.1'
$Tarball = 'hexen2demo_nov1997-linux-i586.tgz'
$Sha256  = '2DF15CDE0128B7A036E71995E068CA853F13BE8E2B591CAAC140025D66643FC0'
# Only this subtree is wanted.  The package also carries 1997 i586 Linux
# binaries of the old engine, which are of no use on Windows or anywhere else.
$Member  = 'hexen2demo_nov1997/data1'

function Die([string]$Message) {
    Write-Host "get_demo.ps1: $Message" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
    Die "no such directory: $Destination"
}
$Destination = (Resolve-Path -LiteralPath $Destination).Path

if (Test-Path -LiteralPath (Join-Path $Destination 'data1\pak0.pak')) {
    Die "$Destination\data1\pak0.pak already exists -- refusing to overwrite it.`nRemove that data1 folder first if you really want to replace it."
}

if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) {
    Die 'tar.exe not found. It ships with Windows 10 1803 and newer; on older Windows, unpack the archive by hand (7-Zip opens .tgz) and copy its data1 folder here.'
}

# Unpack in a scratch folder and move into place, so an interrupted run cannot
# leave a half-populated data1 that the engine would then try to load.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("hexenwail-demo-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

try {
    $archive = Join-Path $work $Tarball

    Write-Host 'Downloading the Hexen II demo (13 MB) from the uHexen2 project...'
    # Invoke-WebRequest's progress bar makes large downloads crawl.
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $archive -UseBasicParsing -UserAgent $UserAgent
    } catch {
        Die "download failed: $($_.Exception.Message)"
    } finally {
        $ProgressPreference = $oldProgress
    }

    Write-Host 'Verifying...'
    $got = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($got -ne $Sha256) {
        Die "checksum mismatch -- refusing to install.`n  expected $Sha256`n  got      $got`nThe download was corrupted, or the file upstream is not the one this script was written against."
    }

    & tar.exe -xzf $archive -C $work --strip-components=1 $Member
    if ($LASTEXITCODE -ne 0) {
        Die "could not extract $Member from the archive."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $work 'data1\pak0.pak'))) {
        Die 'archive did not contain data1/pak0.pak.'
    }

    Move-Item -LiteralPath (Join-Path $work 'data1') -Destination (Join-Path $Destination 'data1')
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Installed the demo data to $Destination\data1"
Write-Host ""
Write-Host 'Launch Hexenwail from that folder and it will pick it up; it should print'
Write-Host '"Playing the demo version." during startup.  If you run the engine from'
Write-Host 'somewhere else, point it here with:'
Write-Host ""
Write-Host "    hexenwail.exe -basedir `"$Destination`""
Write-Host ""
Write-Host 'This is the three-level demo.  The full game unlocks the rest -- copy the'
Write-Host 'data1 folder from a Steam, GOG or disc installation over this one.'
