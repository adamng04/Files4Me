param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$DisplayVersion,
    [Parameter(Mandatory=$true)][string]$Tag,
    [Parameter(Mandatory=$true)][string]$Notes,
    [string]$KeyName = 'Files4Me Update Manifest Signing v1',
    [string]$OutputDirectory = 'updates\alpha'
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Files4Me.ReleaseSigning.psm1') -Force
$root = Split-Path -Parent $PSScriptRoot
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$outputPath = Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$installerItem = Get-Item -LiteralPath $installerPath
$installerName = $installerItem.Name
$sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToUpperInvariant()
$baseUrl = "https://github.com/adamng04/Files4Me"
$safeNotes = ($Notes -replace "[`r`n=]", ' ').Trim()
if (-not $safeNotes) { throw 'Release notes must not be empty.' }

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
