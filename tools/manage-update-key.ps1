param(
    [Parameter(Mandatory=$true)][ValidateSet('Migrate','Audit','TestBackup','Restore')][string]$Action,
    [string]$LegacyKey = (Join-Path $env:USERPROFILE 'Documents\Files4Me-update-private.key'),
    [string[]]$BackupPath,
    [string]$KeyName = 'Files4Me Update Manifest Signing v1'
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Files4Me.ReleaseSigning.psm1') -Force

switch ($Action) {
    'Migrate' {
        if (-not $BackupPath -or $BackupPath.Count -ne 2) {
            throw 'Migrate requires exactly two -BackupPath values on separate offline storage devices.'
        }
        Import-Files4MeLegacyReleaseKey -LegacyKey $LegacyKey -BackupPath $BackupPath -KeyName $KeyName
    }
    'Audit' {
        Get-Files4MeReleaseKeyAudit -KeyName $KeyName
    }
    'TestBackup' {
        if (-not $BackupPath) { throw 'TestBackup requires one or more -BackupPath values.' }
        Test-Files4MeRecoveryBackup -BackupPath $BackupPath
    }
    'Restore' {
        if (-not $BackupPath -or $BackupPath.Count -ne 1) { throw 'Restore requires exactly one -BackupPath value.' }
        Restore-Files4MeReleaseKey -BackupPath $BackupPath[0] -KeyName $KeyName
    }
}
