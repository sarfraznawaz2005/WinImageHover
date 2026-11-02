<#
.SYNOPSIS
  Build ImageHover and publish a GitHub release. Start it with .\release.bat.

.DESCRIPTION
  Release steps:
    1. .\release.bat -SetVersion 1.1.0
         Sets the new version (res\version.h, res\app.manifest) and adds an empty
         "## [1.1.0]" section to CHANGELOG.md.
    2. Write what was Added / Changed / Fixed / Removed in CHANGELOG.md.
    3. Commit and push your changes (you do this yourself).
    4. .\release.bat -DryRun
         Builds, tests and packs, and shows the release notes. Publishes nothing.
    5. .\release.bat
         Same checks, then asks "Type YES" and creates the GitHub release
         (tag vX.Y.Z on the pushed commit, zip + SHA256 attached).

  Needs: MinGW-w64 (gcc, windres), git, GitHub CLI "gh" signed in (gh auth login).

.PARAMETER SetVersion
  New version, for example 1.1.0. Only updates files; builds and publishes nothing.

.PARAMETER DryRun
  Do everything except publishing. Git and GitHub checks only give warnings.

.PARAMETER Draft
  Create the GitHub release as a draft (not public until you publish it on GitHub).
#>
[CmdletBinding()]
param(
    [string]$SetVersion,
    [switch]$DryRun,
    [switch]$Draft
)

# 'Continue': git/gh write normal messages to stderr; PowerShell 5.1 would stop on them.
# Cmdlets that must not fail use -ErrorAction Stop.
$ErrorActionPreference = 'Continue'
$Root = $PSScriptRoot
Set-Location $Root

$VersionH  = Join-Path $Root 'res\version.h'
$Manifest  = Join-Path $Root 'res\app.manifest'
$Changelog = Join-Path $Root 'CHANGELOG.md'
$Exe       = Join-Path $Root 'ImageHover.exe'
$Dist      = Join-Path $Root 'dist'
$Utf8      = New-Object System.Text.UTF8Encoding $false

function Say([string]$msg)  { Write-Host "==> $msg" -ForegroundColor Cyan }
function Warn([string]$msg) { Write-Host "WARNING: $msg" -ForegroundColor Yellow }
function Fail([string]$msg) { Write-Host "STOPPED: $msg" -ForegroundColor Red; exit 1 }
function ReadText([string]$p) { [System.IO.File]::ReadAllText($p) }
function WriteText([string]$p, [string]$t) { [System.IO.File]::WriteAllText($p, $t, $Utf8) }

function Get-AppVersion {
    $t = ReadText $VersionH
    if ($t -notmatch '#define\s+IH_VER_STR\s+"(\d+\.\d+\.\d+)"') { Fail "No IH_VER_STR in res\version.h" }
    return $Matches[1]
}

# Text of the "## [ver]" section, without empty headings and empty bullets.
function Get-ReleaseNotes([string]$ver) {
    $lines = (ReadText $Changelog) -split "`r?`n"
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^## \[$([regex]::Escape($ver))\]") { $start = $i; break }
    }
    if ($start -lt 0) { return $null }
    $body = New-Object System.Collections.Generic.List[string]
    for ($i = $start + 1; $i -lt $lines.Count -and $lines[$i] -notmatch '^## \['; $i++) {
        if ($lines[$i] -match '^\s*-\s*$') { continue }          # empty bullet from the template
        $body.Add($lines[$i])
    }
    # Drop "### Heading" blocks that have no content.
    $out = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $body.Count; $i++) {
        if ($body[$i] -match '^###\s') {
            $j = $i + 1
            $has = $false
            while ($j -lt $body.Count -and $body[$j] -notmatch '^###\s') {
                if ($body[$j].Trim()) { $has = $true }
                $j++
            }
            if (-not $has) { $i = $j - 1; continue }
        }
        $out.Add($body[$i])
    }
    return ($out -join "`n").Trim()
}

# ---------------------------------------------------------------------------
# -SetVersion: update version files and add a CHANGELOG section, then stop.
# ---------------------------------------------------------------------------
if ($SetVersion) {
    if ($SetVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') { Fail "Version must look like 1.2.3" }
    $maj, $min, $pat = $Matches[1], $Matches[2], $Matches[3]
    $old = Get-AppVersion

    $t = ReadText $VersionH
    $t = $t -replace '(#define\s+IH_VER_MAJOR\s+)\d+', "`${1}$maj"
    $t = $t -replace '(#define\s+IH_VER_MINOR\s+)\d+', "`${1}$min"
    $t = $t -replace '(#define\s+IH_VER_PATCH\s+)\d+', "`${1}$pat"
    $t = $t -replace '(#define\s+IH_VER_STR\s+)"[^"]*"', "`${1}`"$SetVersion`""
    WriteText $VersionH $t

    $m = ReadText $Manifest
    $m = $m -replace '(name="ImageHover" version=")[^"]*"', "`${1}$SetVersion.0`""
    WriteText $Manifest $m

    $c = ReadText $Changelog
    if ($c -match "(?m)^## \[$([regex]::Escape($SetVersion))\]") {
        Say "CHANGELOG.md already has a section for $SetVersion."
    } else {
        $date = Get-Date -Format 'yyyy-MM-dd'
        $nl = if ($c -match "`r`n") { "`r`n" } else { "`n" }
        $section = "## [$SetVersion] - $date$nl$nl### Added$nl- $nl$nl### Changed$nl- $nl$nl### Fixed$nl- $nl$nl### Removed$nl- $nl$nl"
        $at = $c.IndexOf("## [")
        if ($at -lt 0) { $c = $c.TrimEnd() + $nl + $nl + $section } else { $c = $c.Insert($at, $section) }
        WriteText $Changelog $c
    }
    Say "Version changed: $old -> $SetVersion"
    Write-Host "Next: write the changes under '## [$SetVersion]' in CHANGELOG.md, commit, push, then run .\release.bat"
    exit 0
}

# ---------------------------------------------------------------------------
# Release
# ---------------------------------------------------------------------------
$ver = Get-AppVersion
$tag = "v$ver"
Say "Release $tag$(if ($DryRun) { ' (dry run: nothing is published)' })"

# 1. Release notes from CHANGELOG.md
$notes = Get-ReleaseNotes $ver
if ($null -eq $notes) { Fail "CHANGELOG.md has no '## [$ver]' section. Run: .\release.bat -SetVersion $ver" }
if ($notes -notmatch '(?m)^\s*-\s+\S') { Fail "The '## [$ver]' section in CHANGELOG.md has no entries. Write what changed first." }

# 2. Git and GitHub checks (warnings only in a dry run)
function Check([bool]$ok, [string]$msg) {
    if ($ok) { return }
    if ($DryRun) { Warn $msg } else { Fail $msg }
}
$haveGit = [bool](Get-Command git -ErrorAction SilentlyContinue)
$haveGh  = [bool](Get-Command gh  -ErrorAction SilentlyContinue)
Check $haveGit "git is not installed."
Check $haveGh  "GitHub CLI 'gh' is not installed. Get it from https://cli.github.com"
$commit = $null
if ($haveGit) {
    $null = git rev-parse --is-inside-work-tree 2>$null
    $inRepo = ($LASTEXITCODE -eq 0)
    Check $inRepo "This folder is not a git repository."
    if ($inRepo) {
        $dirty = git status --porcelain
        Check (-not $dirty) "You have changes that are not committed. Commit and push them first."
        $null = git rev-parse --abbrev-ref '@{u}' 2>$null
        $hasUp = ($LASTEXITCODE -eq 0)
        Check $hasUp "The current branch has no upstream on GitHub. Push it first (git push -u origin <branch>)."
        if ($hasUp) {
            $ahead = [int](git rev-list --count '@{u}..HEAD')
            Check ($ahead -eq 0) "$ahead commit(s) are not pushed yet. Push them first."
        }
        $commit = (git rev-parse HEAD 2>$null)
        $null = git rev-parse -q --verify "refs/tags/$tag" 2>$null
        Check ($LASTEXITCODE -ne 0) "Tag $tag already exists locally. Raise the version: .\release.bat -SetVersion x.y.z"
    }
}
if ($haveGh) {
    $null = gh auth status 2>&1
    $authed = ($LASTEXITCODE -eq 0)
    Check $authed "GitHub CLI is not signed in. Run: gh auth login"
    if ($authed -and $commit) {
        $null = gh release view $tag 2>&1
        Check ($LASTEXITCODE -ne 0) "A GitHub release $tag already exists. Raise the version: .\release.bat -SetVersion x.y.z"
    }
}

# 3. Build (stop a running copy first, the exe file is locked while it runs)
$running = Get-Process ImageHover -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }
if ($running) { Say "Closing the running ImageHover so the exe can be rebuilt"; $running | Stop-Process -Force; Start-Sleep -Milliseconds 500 }
Say "Building"
cmd /c "`"$Root\build.bat`""
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Exe)) { Fail "Build failed." }
$exeVer = (Get-Item $Exe).VersionInfo.ProductVersion
if ($exeVer -ne $ver) { Fail "ImageHover.exe says version '$exeVer', expected '$ver'." }

# 4. Tests
Say "Running tests"
cmd /c "`"$Root\tools\test_loader.bat`""
$testCode = $LASTEXITCODE
Remove-Item (Join-Path $Root 'tools\out') -Recurse -Force -ErrorAction SilentlyContinue
if ($testCode -ne 0) { Fail "Tests failed." }

# 5. Package
Say "Packing"
New-Item -ItemType Directory -Force $Dist -ErrorAction Stop | Out-Null
$zipName = "ImageHover-$ver-win-x64.zip"
$zip = Join-Path $Dist $zipName
$stage = Join-Path $Dist "stage-$ver"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $zip -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage -ErrorAction Stop | Out-Null
Copy-Item $Exe, (Join-Path $Root 'README.md'), $Changelog -Destination $stage -ErrorAction Stop
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -ErrorAction Stop
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
$shaFile = "$zip.sha256"
WriteText $shaFile "$hash  $zipName`n"

$fullNotes = @"
$notes

### Download
1. Download **$zipName** below and unzip it.
2. Run ``ImageHover.exe``. No install needed. Windows 10 or 11, 64-bit.

SHA256 of ``$zipName``:
``````
$hash
``````
"@
$notesFile = Join-Path $Dist "release-notes-$ver.md"
WriteText $notesFile $fullNotes

Write-Host ""
Write-Host "---------------- Release notes ----------------"
Write-Host $fullNotes
Write-Host "-----------------------------------------------"
Write-Host "Files: $zip"
Write-Host "       $shaFile"
Write-Host ""

if ($DryRun) { Say "Dry run finished. Nothing was published."; exit 0 }

# 6. Publish
$kind = if ($Draft) { 'a DRAFT release' } else { 'a PUBLIC release' }
$answer = Read-Host "Create $kind $tag on GitHub from commit $($commit.Substring(0,7))? Type YES to continue"
if ($answer -cne 'YES') { Say "Cancelled. Nothing was published."; exit 1 }

$ghArgs = @('release', 'create', $tag, $zip, $shaFile,
            '--title', "ImageHover $ver",
            '--notes-file', $notesFile,
            '--target', $commit)
if ($Draft) { $ghArgs += '--draft' }
gh @ghArgs
if ($LASTEXITCODE -ne 0) { Fail "gh release create failed." }
Say "Done. Release $tag is on GitHub."
