$ErrorActionPreference = 'Stop'

$buildPath = $env:PATH
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('PATH', $buildPath, 'Process')

$projectRoot = Split-Path -Parent $PSScriptRoot
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild was not found at $msbuild"
}
if (-not $iscc) {
    throw 'Inno Setup 6 was not found. Install JRSoftware.InnoSetup, then retry.'
}

Push-Location $projectRoot
try {
    & $msbuild 'Files4Me.sln' '/m' '/p:Configuration=Release' '/p:Platform=x64'
    if ($LASTEXITCODE -ne 0) { throw "Files4Me build failed with exit code $LASTEXITCODE" }

    & $iscc 'installer\Files4Me.iss'
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
