param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$DisplayVersion,
    [Parameter(Mandatory=$true)][string]$Tag,
    [Parameter(Mandatory=$true)][string]$Notes,
    [Parameter(Mandatory=$true)][ValidateSet('alpha', 'stable')][string]$Channel,
    [string]$KeyName = 'Files4Me Update Manifest Signing v1'
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Files4Me.ReleaseSigning.psm1') -Force
$root = Split-Path -Parent $PSScriptRoot
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$outputPath = Join-Path $root (Join-Path 'updates' $Channel)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$installerItem = Get-Item -LiteralPath $installerPath
$installerName = $installerItem.Name
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$') {
    throw 'Version must be a strict SemVer value without build metadata.'
}
$prerelease = ($Version -split '-', 2)
if ($prerelease.Count -eq 2) {
    foreach ($identifier in ($prerelease[1] -split '\.')) {
        if ($identifier -cmatch '^[0-9]+$' -and $identifier.Length -gt 1 -and $identifier[0] -eq '0') {
            throw 'Numeric prerelease identifiers must not contain leading zeroes.'
        }
    }
}
if ($DisplayVersion -cnotmatch '^[0-9A-Za-z](?:[0-9A-Za-z._-]{0,62}[0-9A-Za-z])?$') {
    throw 'DisplayVersion must be 1-64 filename-safe characters and start/end with a letter or digit.'
}
if ($Tag -cnotmatch '^v[0-9A-Za-z][0-9A-Za-z._-]{0,63}$') { throw 'Tag is not a safe GitHub release tag.' }
if ($installerName -cnotmatch '^[0-9A-Za-z][0-9A-Za-z._-]*\.exe$') { throw 'Installer filename is not URL-safe.' }
$sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToUpperInvariant()
$baseUrl = "https://github.com/adamng04/Files4Me"
$safeNotes = ($Notes -replace '[\x00-\x1F=]', ' ').Trim()
if (-not $safeNotes) { throw 'Release notes must not be empty.' }
if ($safeNotes.Length -gt 2048) { throw 'Release notes must not exceed 2048 characters.' }

$manifest = @(
    'schema=1'
    "version=$Version"
    "display_version=$DisplayVersion"
    "installer_url=$baseUrl/releases/download/$Tag/$installerName"
    "release_url=$baseUrl/releases/tag/$Tag"
    "sha256=$sha256"
    "size=$($installerItem.Length)"
    "notes=$safeNotes"
) -join "`n"
$manifest += "`n"
$utf8 = [Text.UTF8Encoding]::new($false)
$manifestBytes = $utf8.GetBytes($manifest)

$sha = [System.Security.Cryptography.SHA256]::Create()
try { $digest = $sha.ComputeHash($manifestBytes) } finally { $sha.Dispose() }
$signature = Invoke-Files4MeReleaseSignature -Digest $digest -KeyName $KeyName

$manifestPath = Join-Path $outputPath 'manifest.ini'
$signaturePath = Join-Path $outputPath 'manifest.ini.sig'
[IO.File]::WriteAllBytes($manifestPath, $manifestBytes)
[IO.File]::WriteAllText($signaturePath, [Convert]::ToBase64String($signature) + "`n", [Text.Encoding]::ASCII)

[pscustomobject]@{
    Manifest = $manifestPath
    Signature = $signaturePath
    Installer = $installerPath
    SHA256 = $sha256
    Size = $installerItem.Length
}
