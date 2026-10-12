<#
.SYNOPSIS
    Build a versioned release of one app.

.DESCRIPTION
    Sets LINK_VERSION - which the app's CMakeLists reads, and which also
    marks the build as a release so the splash drops the build stamp -
    then does a full clean build, packages the app binary and tags the
    commit <App>/v<Version>. The tag is local: push it yourself.

    Refuses to run on a tree with uncommitted changes, on a version that
    is not PROJECT_VER_BASE or a pre-release of it, or on a tag that
    already exists.

    Only the application image is packaged. Bootloader and partition
    table are flashed once at the factory; updates over OTA or over USB
    replace the app alone.

.PARAMETER App
    Directory name under apps/, e.g. link-lr. The project
    name, and so the binary name, is the same string.

.PARAMETER Version
    The version string, used as given. With PROJECT_VER_BASE at 1.0.0:
    1.0.0, 1.0.0-rc1.

.EXAMPLE
    .\tools\release.ps1 link-lr 1.0.0

.EXAMPLE
    .\tools\release.ps1 link-lr 1.0.0-rc1
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$App,

    [Parameter(Mandatory = $true, Position = 1)]
    [ValidateNotNullOrEmpty()]
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$repoRoot    = Split-Path -Parent $PSScriptRoot
$projectRoot = Join-Path $repoRoot "apps/$App"
$buildDir    = Join-Path $projectRoot 'build'
$releaseDir  = Join-Path $projectRoot "releases/v$Version"

if (-not (Test-Path (Join-Path $projectRoot 'CMakeLists.txt'))) {
    $known = (Get-ChildItem (Join-Path $repoRoot 'apps') -Directory |
              Select-Object -ExpandProperty Name) -join ', '
    throw "No app named '$App' under apps/. Known apps: $known"
}

$cmakeLists = Join-Path $projectRoot 'CMakeLists.txt'
$base = Select-String -Path $cmakeLists -Pattern 'set\(PROJECT_VER_BASE\s+"([^"]+)"\)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Select-Object -First 1
if (-not $base) {
    throw "PROJECT_VER_BASE not found in apps/$App/CMakeLists.txt"
}
if ($Version -ne $base -and -not $Version.StartsWith("$base-")) {
    throw "Version $Version does not match PROJECT_VER_BASE $base. Bump it in apps/$App/CMakeLists.txt and commit first."
}

$dirty = git -C $repoRoot status --porcelain
if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
if ($dirty) {
    throw "Uncommitted changes - a release is built from a commit:`n$($dirty -join "`n")"
}

$tag = "$App/v$Version"
git -C $repoRoot rev-parse -q --verify "refs/tags/$tag" | Out-Null
if ($LASTEXITCODE -eq 0) { throw "Tag $tag already exists." }

#
# The IDF environment. An already-exported shell is used as it stands;
# otherwise the pieces are found under the standard install locations,
# because export.ps1 will not run on a machine whose system Python is
# newer than the one the IDF venv was built with.
#
# Override IDF_PATH, or IDF_TOOLS_PATH, to point at another install.
#
if (-not $env:IDF_PATH) {
    $roots = @((Join-Path $HOME 'esp'), 'C:\esp') |
             Where-Object { Test-Path $_ }

    $candidates = $roots |
                  ForEach-Object { Get-ChildItem -Path $_ -Directory -ErrorAction SilentlyContinue } |
                  Where-Object { Test-Path (Join-Path $_.FullName 'esp-idf\tools\idf.py') } |
                  Sort-Object Name -Descending

    if (-not $candidates) {
        throw 'IDF_PATH is not set and no ESP-IDF install was found under ~/esp or C:\esp. Set IDF_PATH, or run this from an exported IDF shell.'
    }
    $env:IDF_PATH = Join-Path $candidates[0].FullName 'esp-idf'
}

$idf = Join-Path $env:IDF_PATH 'tools\idf.py'
if (-not (Test-Path $idf)) {
    throw "idf.py not found at $idf - check IDF_PATH."
}

#
# Where the toolchains live. The 6.x installer puts them under
# C:\Espressif\tools, the 5.x one under ~/.espressif; the constraints
# file idf.py reads is picked by IDF_TOOLS_PATH, so guessing wrong here
# fails the build rather than merely slowing it down.
#
$idfVer = (Get-Content (Join-Path $env:IDF_PATH 'tools\cmake\version.cmake') -ErrorAction SilentlyContinue |
           Select-String -Pattern 'IDF_VERSION_(MAJOR|MINOR)\s+(\d+)' -AllMatches |
           ForEach-Object { $_.Matches.Groups[2].Value }) -join '.'

# The 6.x component manager reads this one and raises on a bare
# environment rather than falling back to the IDF it was started from.
$espIdfVersionWasSet = [bool]$env:ESP_IDF_VERSION
if (-not $espIdfVersionWasSet -and $idfVer) {
    $env:ESP_IDF_VERSION = $idfVer
}

if (-not $env:IDF_TOOLS_PATH) {
    # Matched against the IDF version in hand: two installs side by side
    # each carry their own constraints file, and the wrong one is an
    # error rather than a fallback.
    $candidateRoots = @('C:\Espressif\tools', (Join-Path $HOME '.espressif')) |
                      Where-Object { Test-Path $_ }

    $env:IDF_TOOLS_PATH =
        @($candidateRoots | Where-Object { $idfVer -and (Test-Path (Join-Path $_ "espidf.constraints.v$idfVer.txt")) }) +
        @($candidateRoots | Where-Object { Test-Path (Join-Path $_ 'espidf.constraints.v*.txt') }) |
        Select-Object -First 1

    if (-not $env:IDF_TOOLS_PATH) {
        throw 'No ESP-IDF tools directory found. Set IDF_TOOLS_PATH, or run this from an exported IDF shell.'
    }
}

$toolsRoot = $env:IDF_TOOLS_PATH

#
# The venv moved between installer generations: 5.x names it after the
# IDF and Python versions under python_env/, 6.x puts it under
# python/<idf version>/venv. Both are matched rather than spelled out.
#
if (-not $env:IDF_PYTHON_ENV_PATH) {
    # python/ also holds the interpreter's own DLLs, Lib and Scripts, so
    # only version-shaped names are considered - and each candidate has
    # to actually carry a python.exe.
    $venvRoot =
        @(Get-ChildItem -Path (Join-Path $toolsRoot 'python_env') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName }) +
        @(Get-ChildItem -Path (Join-Path $toolsRoot 'python') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^v?\d+(\.\d+)*$' } |
            ForEach-Object { Join-Path $_.FullName 'venv' }) |
        Where-Object { Test-Path (Join-Path $_ 'Scripts\python.exe') } |
        Sort-Object -Descending | Select-Object -First 1

    if (-not $venvRoot) {
        throw "No IDF Python environment found under $toolsRoot. Run the IDF installer, or set IDF_PYTHON_ENV_PATH."
    }
    $env:IDF_PYTHON_ENV_PATH = $venvRoot
}

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path $python)) {
    throw "IDF Python not found at $python - check IDF_PYTHON_ENV_PATH."
}

# Only needed by the full reconfigure a clean build forces: CMake then
# looks for the compiler on the PATH. Missing entries are harmless.
$paths = @(
    (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts')
    (Get-ChildItem -Path (Join-Path $toolsRoot 'tools\xtensa-esp-elf') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName 'xtensa-esp-elf\bin' })
    (Get-ChildItem -Path (Join-Path $toolsRoot 'xtensa-esp-elf') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName 'xtensa-esp-elf\bin' })
    (Get-ChildItem -Path (Join-Path $toolsRoot 'tools\ninja') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1 |
        ForEach-Object { $_.FullName })
    (Get-ChildItem -Path (Join-Path $toolsRoot 'ninja') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1 |
        ForEach-Object { $_.FullName })
) | Where-Object { $_ -and (Test-Path $_) }

$env:PATH = ($paths -join ';') + ';' + $env:PATH

if (-not $env:ESP_ROM_ELF_DIR) {
    $romDir = @(Get-ChildItem -Path (Join-Path $toolsRoot 'tools\esp-rom-elfs') -Directory -ErrorAction SilentlyContinue) +
              @(Get-ChildItem -Path (Join-Path $toolsRoot 'esp-rom-elfs') -Directory -ErrorAction SilentlyContinue) |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($romDir) { $env:ESP_ROM_ELF_DIR = $romDir.FullName }
}

Write-Host ''
Write-Host '============================================================'
Write-Host " $App release build : v$Version"
Write-Host '============================================================'
Write-Host ''

# Read by the app's CMakeLists. Also what sets LINK_IS_RELEASE, which
# takes the build stamp off the splash.
$env:LINK_VERSION = $Version

Push-Location $projectRoot
try {
    # Removed outright rather than through `idf.py fullclean`: that
    # empties the directory but leaves it behind, and refuses to touch
    # what it no longer recognises as a CMake build tree - so it fails
    # on the second release in a row.
    #
    # An interrupted idf.py can leave a log file open under build/log,
    # and Windows will then refuse to delete - or even rename - the
    # whole tree. Everything else still goes, which is enough: what
    # matters is that no build artefact survives into the release.
    Write-Host '[1/3] Clean...'
    if (Test-Path $buildDir) {
        $locked = 0
        Get-ChildItem $buildDir -Recurse -File | ForEach-Object {
            try { Remove-Item $_.FullName -Force -ErrorAction Stop }
            catch { $locked++ }
        }

        if ($locked -eq 0) {
            Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
        }
        else {
            Write-Host "      $locked locked log file(s) left behind - harmless"
        }
    }

    Write-Host ''
    Write-Host "[2/3] Build (LINK_VERSION=$Version)..."
    & $python $idf build
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }

    Write-Host ''
    Write-Host "[3/3] Packaging into $releaseDir"

    $appBin = Join-Path $buildDir "$App.bin"
    if (-not (Test-Path $appBin)) {
        throw "app binary not found at $appBin"
    }

    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

    $target = Join-Path $releaseDir "$App-v$Version.bin"
    Copy-Item -Path $appBin -Destination $target -Force

    # The SHA-256 is what the app checks an OTA image against, so it is
    # part of the release rather than something recomputed later.
    $sha  = (Get-FileHash -Path $target -Algorithm SHA256).Hash.ToLower()
    $size = (Get-Item $target).Length

    # Straight from the table that was just built, so the manifest cannot
    # drift from where the image actually goes.
    $otaOffset = Select-String -Path (Join-Path $projectRoot 'partitions.csv') `
                               -Pattern '^\s*ota_0\s*,' -ErrorAction SilentlyContinue |
                 ForEach-Object { ($_.Line -split ',')[3].Trim() } |
                 Select-Object -First 1

    $slot = if ($otaOffset) { "the OTA slot ($otaOffset for ota_0)" } else { 'the app slot' }

    @(
        "$App release v$Version"
        "Built  : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        ''
        "App firmware, for $slot`:"
        "  $App-v$Version.bin"
        "  size   : $size bytes"
        "  sha256 : $sha"
    ) | Set-Content -Path (Join-Path $releaseDir 'MANIFEST.txt') -Encoding UTF8

    git -C $repoRoot tag -a $tag -m "$App v$Version" -m "sha256 $sha"
    if ($LASTEXITCODE -ne 0) { throw "tagging $tag failed" }

    Write-Host ''
    Write-Host '============================================================'
    Write-Host " SUCCESS : $releaseDir"
    Write-Host " Tag     : $tag (local)"
    Write-Host '============================================================'
    Get-ChildItem $releaseDir | Select-Object -ExpandProperty Name
    Write-Host ''
    Write-Host "Next: git push origin $tag"
    Write-Host "      then bump PROJECT_VER_BASE in apps/$App/CMakeLists.txt"
    Write-Host ''
}
finally {
    Pop-Location
    Remove-Item Env:\LINK_VERSION -ErrorAction SilentlyContinue
    if (-not $espIdfVersionWasSet) {
        Remove-Item Env:\ESP_IDF_VERSION -ErrorAction SilentlyContinue
    }
}
