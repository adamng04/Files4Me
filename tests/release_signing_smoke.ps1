param([switch]$HighProtection)

$ErrorActionPreference = 'Stop'
Set-ExecutionPolicy -Scope Process Bypass -Force
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\Files4Me.ReleaseSigning.psm1') -Force

$module = Get-Module Files4Me.ReleaseSigning
$keyName = 'Files4Me Release Signing Smoke ' + [guid]::NewGuid().ToString('N')
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(3072)
$rsa.PersistKeyInCsp = $false
$legacy = $null
$pkcs8 = $null
$restored = $null
$encrypted = $null
try {
    $legacy = $rsa.ExportCspBlob($true)
    $pkcs8 = [Files4Me.ReleaseKeyNative]::LegacyBlobToPkcs8($legacy)
    $fingerprint = [Files4Me.ReleaseKeyNative]::Pkcs8Fingerprint($pkcs8)
    if ($fingerprint.Length -ne 64) { throw 'Unexpected public-key fingerprint.' }

    $passphrase = ConvertTo-SecureString 'Files4Me smoke backup passphrase 2026' -AsPlainText -Force
    $encrypted = & $module { param($bytes,$secret) Protect-Files4MePkcs8 -Pkcs8 $bytes -Passphrase $secret } $pkcs8 $passphrase
    if ([Text.Encoding]::ASCII.GetString($encrypted) -notmatch 'BEGIN ENCRYPTED PRIVATE KEY') {
        throw 'OpenSSL did not create encrypted PKCS#8 PEM.'
    }
    $asn1Bytes = & $module { param($bytes,$secret) Invoke-OpenSslBytes -Arguments @('asn1parse','-inform','PEM') -InputBytes $bytes -Passphrase $secret } $encrypted $passphrase
    $asn1 = [Text.Encoding]::UTF8.GetString($asn1Bytes)
    foreach ($expected in @('PBES2','PBKDF2','hmacWithSHA256','aes-256-cbc','0927C0')) {
        if ($asn1 -notmatch [regex]::Escape($expected)) { throw "Encrypted PKCS#8 is missing $expected." }
    }
    $restored = & $module { param($bytes,$secret) Unprotect-Files4MePkcs8 -EncryptedPem $bytes -Passphrase $secret } $encrypted $passphrase
    if ([Files4Me.ReleaseKeyNative]::Pkcs8Fingerprint($restored) -cne $fingerprint) {
        throw 'Recovery backup changed the public key.'
    }

    $wrong = ConvertTo-SecureString 'definitely the wrong passphrase' -AsPlainText -Force
    $wrongRejected = $false
    try { & $module { param($bytes,$secret) Unprotect-Files4MePkcs8 -EncryptedPem $bytes -Passphrase $secret } $encrypted $wrong | Out-Null }
    catch { $wrongRejected = $true }
    if (-not $wrongRejected) { throw 'Wrong recovery passphrase was accepted.' }

    [Files4Me.ReleaseKeyNative]::ImportSoftwareKey($pkcs8, $keyName, [bool]$HighProtection)
    $audit = [Files4Me.ReleaseKeyNative]::AuditSoftwareKey($keyName)
    if ($audit.KeySize -ne 3072 -or -not $audit.NonExportable -or $audit.PublicFingerprint -cne $fingerprint) {
        throw 'Persisted CNG key audit failed.'
    }
    if ($HighProtection -and -not $audit.HighProtection) { throw 'Strong-use policy was not persisted.' }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes('Files4Me signing smoke challenge')) }
    finally { $sha.Dispose() }
    $signature = [Files4Me.ReleaseKeyNative]::SignSoftwareKey($keyName, $digest)
    if ($signature.Length -ne 384) { throw 'CNG signer returned an unexpected signature length.' }
    $publicBlob = [Files4Me.ReleaseKeyNative]::Pkcs8PublicBlob($pkcs8)
    if (-not [Files4Me.ReleaseKeyNative]::VerifyWithBCrypt($publicBlob, $digest, $signature)) {
        throw 'BCrypt rejected the CNG signature.'
    }
    $tamperedDigest = [byte[]]$digest.Clone()
    $tamperedDigest[0] = $tamperedDigest[0] -bxor 1
    if ([Files4Me.ReleaseKeyNative]::VerifyWithBCrypt($publicBlob, $tamperedDigest, $signature)) {
        throw 'BCrypt accepted a tampered digest.'
    }

    [pscustomobject]@{
        Pkcs8Encryption = 'Passed'
        Pbkdf2Iterations = 600000
        WrongPassphrase = 'Rejected'
        CngPersistedImport = 'Passed'
        NonExportable = $audit.NonExportable
        HighProtection = $audit.HighProtection
        SignatureBytes = $signature.Length
        BCryptCompatibility = 'Passed'
        PublicFingerprint = $fingerprint
    }
}
finally {
    if ([Files4Me.ReleaseKeyNative]::KeyExists($keyName)) { [Files4Me.ReleaseKeyNative]::DeleteSoftwareKey($keyName) }
    $rsa.Clear()
    $rsa.Dispose()
    foreach ($buffer in @($legacy,$pkcs8,$restored,$encrypted)) {
        if ($buffer) { [Array]::Clear($buffer, 0, $buffer.Length) }
    }
}
