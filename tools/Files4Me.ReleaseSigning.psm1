Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:DefaultKeyName = 'Files4Me Update Manifest Signing v1'
$script:SoftwareProvider = 'Microsoft Software Key Storage Provider'
$script:ExpectedPublicFingerprint = '8F4E35B459D10444D8FB4B3308931872A57B4C219AACFA5B42C446899F6F69D7'
$script:Pkcs8Iterations = 600000
$script:OpenSslPath = $null

if (-not ('Files4Me.ReleaseKeyNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace Files4Me {
    public sealed class ReleaseKeyAudit {
        public string KeyName { get; set; }
        public string Provider { get; set; }
        public string Algorithm { get; set; }
        public int KeySize { get; set; }
        public int ExportPolicy { get; set; }
        public int UiPolicy { get; set; }
        public string PublicFingerprint { get; set; }
        public bool NonExportable { get { return ExportPolicy == 0; } }
        public bool HighProtection { get { return (UiPolicy & 2) != 0; } }
    }

    public static class ReleaseKeyNative {
        private const string ProviderName = "Microsoft Software Key Storage Provider";
        private const string Pkcs8Blob = "PKCS8_PRIVATEKEY";
        private const string PublicBlobType = "RSAPUBLICBLOB";
        private const int PkcsKeyName = 45;
        private const int DoNotFinalize = 0x00000400;
        private const int Persist = unchecked((int)0x80000000);
        private const int PadPkcs1 = 0x00000002;
        private const int UiProtect = 0x00000001;
        private const int UiForceHigh = 0x00000002;

        [StructLayout(LayoutKind.Sequential)]
        private struct NCryptBuffer {
            public int cbBuffer;
            public int BufferType;
            public IntPtr pvBuffer;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NCryptBufferDesc {
            public int ulVersion;
            public int cBuffers;
            public IntPtr pBuffers;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NCryptUiPolicy {
            public int dwVersion;
            public int dwFlags;
            public IntPtr pszCreationTitle;
            public IntPtr pszFriendlyName;
            public IntPtr pszDescription;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Pkcs1PaddingInfo {
            public IntPtr pszAlgId;
        }

        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptOpenStorageProvider(out IntPtr provider, string name, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptOpenKey(IntPtr provider, out IntPtr key, string name, int legacySpec, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptCreatePersistedKey(IntPtr provider, out IntPtr key, string algorithm,
            string name, int legacySpec, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptImportKey(IntPtr provider, IntPtr importKey, string blobType,
            ref NCryptBufferDesc parameters, out IntPtr key, byte[] data, int dataLength, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptSetProperty(IntPtr handle, string property, byte[] input, int inputLength, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptSetProperty(IntPtr handle, string property, IntPtr input, int inputLength, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptGetProperty(IntPtr handle, string property, byte[] output, int outputLength, out int result, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptFinalizeKey(IntPtr key, int flags);
        [DllImport("ncrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int NCryptExportKey(IntPtr key, IntPtr exportKey, string blobType, IntPtr parameters,
            byte[] output, int outputLength, out int result, int flags);
        [DllImport("ncrypt.dll")]
        private static extern int NCryptSignHash(IntPtr key, ref Pkcs1PaddingInfo padding, byte[] hash, int hashLength,
            byte[] signature, int signatureLength, out int result, int flags);
        [DllImport("ncrypt.dll")]
        private static extern int NCryptVerifySignature(IntPtr key, ref Pkcs1PaddingInfo padding, byte[] hash, int hashLength,
            byte[] signature, int signatureLength, int flags);
        [DllImport("ncrypt.dll")]
        private static extern int NCryptDeleteKey(IntPtr key, int flags);
        [DllImport("ncrypt.dll")]
        private static extern int NCryptFreeObject(IntPtr handle);
        [DllImport("bcrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int BCryptOpenAlgorithmProvider(out IntPtr algorithm, string algorithmId, string implementation, int flags);
        [DllImport("bcrypt.dll", CharSet = CharSet.Unicode)]
        private static extern int BCryptImportKeyPair(IntPtr algorithm, IntPtr importKey, string blobType,
            out IntPtr key, byte[] input, int inputLength, int flags);
        [DllImport("bcrypt.dll")]
        private static extern int BCryptVerifySignature(IntPtr key, ref Pkcs1PaddingInfo padding, byte[] hash, int hashLength,
            byte[] signature, int signatureLength, int flags);
        [DllImport("bcrypt.dll")]
        private static extern int BCryptDestroyKey(IntPtr key);
        [DllImport("bcrypt.dll")]
        private static extern int BCryptCloseAlgorithmProvider(IntPtr algorithm, int flags);

        private static void Check(int status, string operation) {
            if (status != 0) throw new CryptographicException(operation + " failed (0x" + status.ToString("X8") + ").");
        }

        private static byte[] Join(params byte[][] pieces) {
            int size = 0;
            foreach (byte[] piece in pieces) size = checked(size + piece.Length);
            byte[] output = new byte[size];
            int offset = 0;
            foreach (byte[] piece in pieces) {
                Buffer.BlockCopy(piece, 0, output, offset, piece.Length);
                offset += piece.Length;
            }
            return output;
        }

        private static byte[] DerLength(int value) {
            if (value < 0x80) return new byte[] { (byte)value };
            List<byte> bytes = new List<byte>();
            while (value > 0) { bytes.Insert(0, (byte)(value & 0xff)); value >>= 8; }
            bytes.Insert(0, (byte)(0x80 | bytes.Count));
            return bytes.ToArray();
        }

        private static byte[] Der(byte tag, byte[] content) {
            return Join(new byte[] { tag }, DerLength(content.Length), content);
        }

        private static byte[] Integer(byte[] value) {
            int start = 0;
            while (start + 1 < value.Length && value[start] == 0) start++;
            byte[] unsigned = new byte[value.Length - start];
            Buffer.BlockCopy(value, start, unsigned, 0, unsigned.Length);
            if ((unsigned[0] & 0x80) != 0) unsigned = Join(new byte[] { 0 }, unsigned);
            return Der(0x02, unsigned);
        }

        private static byte[] EncodePkcs8(RSAParameters p) {
            byte[] pkcs1 = Der(0x30, Join(
                Integer(new byte[] { 0 }), Integer(p.Modulus), Integer(p.Exponent), Integer(p.D),
                Integer(p.P), Integer(p.Q), Integer(p.DP), Integer(p.DQ), Integer(p.InverseQ)));
            byte[] rsaAlgorithm = new byte[] { 0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00 };
            return Der(0x30, Join(new byte[] { 0x02,0x01,0x00 }, rsaAlgorithm, Der(0x04, pkcs1)));
        }

        private sealed class DerReader {
            private readonly byte[] data;
            private int position;
            public DerReader(byte[] bytes) { data = bytes; }
            public byte[] Read(byte expectedTag) {
                if (position >= data.Length || data[position++] != expectedTag) throw new CryptographicException("Invalid PKCS#8 structure.");
                if (position >= data.Length) throw new CryptographicException("Invalid DER length.");
                int length = data[position++];
                if ((length & 0x80) != 0) {
                    int count = length & 0x7f;
                    if (count == 0 || count > 4 || position + count > data.Length) throw new CryptographicException("Invalid DER length.");
                    length = 0;
                    for (int i = 0; i < count; i++) length = checked((length << 8) | data[position++]);
                }
                if (length < 0 || position + length > data.Length) throw new CryptographicException("Truncated DER value.");
                byte[] result = new byte[length];
                Buffer.BlockCopy(data, position, result, 0, length);
                position += length;
                return result;
            }
            public bool AtEnd { get { return position == data.Length; } }
            public byte PeekTag { get {
                if (position >= data.Length) throw new CryptographicException("Truncated DER value.");
                return data[position];
            } }
        }

        private static byte[] UnsignedInteger(byte[] value) {
            if (value.Length == 0 || (value[0] & 0x80) != 0) throw new CryptographicException("Negative RSA integer.");
            int start = 0;
            while (start + 1 < value.Length && value[start] == 0) start++;
            byte[] result = new byte[value.Length - start];
            Buffer.BlockCopy(value, start, result, 0, result.Length);
            return result;
        }

        private static RSAParameters DecodePkcs8(byte[] pkcs8) {
            DerReader top = new DerReader(pkcs8);
            DerReader outer = new DerReader(top.Read(0x30));
            outer.Read(0x02);
            DerReader rsa;
            if (outer.PeekTag == 0x30) {
                DerReader algorithm = new DerReader(outer.Read(0x30));
                byte[] oid = algorithm.Read(0x06);
                byte[] rsaOid = new byte[] { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
                if (oid.Length != rsaOid.Length) throw new CryptographicException("PKCS#8 key is not RSA.");
                for (int i = 0; i < oid.Length; i++) if (oid[i] != rsaOid[i]) throw new CryptographicException("PKCS#8 key is not RSA.");
                rsa = new DerReader(new DerReader(outer.Read(0x04)).Read(0x30));
                rsa.Read(0x02);
            } else {
                // OpenSSL's DER private-key output can be the inner PKCS#1 structure.
                rsa = outer;
            }
            RSAParameters p = new RSAParameters();
            p.Modulus = UnsignedInteger(rsa.Read(0x02));
            p.Exponent = UnsignedInteger(rsa.Read(0x02));
            p.D = UnsignedInteger(rsa.Read(0x02));
            p.P = UnsignedInteger(rsa.Read(0x02));
            p.Q = UnsignedInteger(rsa.Read(0x02));
            p.DP = UnsignedInteger(rsa.Read(0x02));
            p.DQ = UnsignedInteger(rsa.Read(0x02));
            p.InverseQ = UnsignedInteger(rsa.Read(0x02));
            return p;
        }

        private static byte[] PublicBlob(RSAParameters p) {
            int bits = checked(p.Modulus.Length * 8);
            byte[] output = new byte[24 + p.Exponent.Length + p.Modulus.Length];
            WriteInt(output, 0, unchecked((int)0x31415352));
            WriteInt(output, 4, bits);
            WriteInt(output, 8, p.Exponent.Length);
            WriteInt(output, 12, p.Modulus.Length);
            WriteInt(output, 16, 0);
            WriteInt(output, 20, 0);
            Buffer.BlockCopy(p.Exponent, 0, output, 24, p.Exponent.Length);
            Buffer.BlockCopy(p.Modulus, 0, output, 24 + p.Exponent.Length, p.Modulus.Length);
            return output;
        }

        private static byte[] PadLeft(byte[] value, int length) {
            if (value.Length > length) throw new CryptographicException("RSA component is too large.");
            if (value.Length == length) return value;
            byte[] padded = new byte[length];
            Buffer.BlockCopy(value, 0, padded, length - value.Length, value.Length);
            return padded;
        }

        private static byte[] FullPrivateBlob(RSAParameters p) {
            int modulusLength = p.Modulus.Length;
            int prime1Length = p.P.Length;
            int prime2Length = p.Q.Length;
            byte[] output = new byte[24 + p.Exponent.Length + modulusLength +
                prime1Length + prime2Length + prime1Length + prime2Length + prime1Length + modulusLength];
            WriteInt(output, 0, unchecked((int)0x33415352));
            WriteInt(output, 4, checked(modulusLength * 8));
            WriteInt(output, 8, p.Exponent.Length);
            WriteInt(output, 12, modulusLength);
            WriteInt(output, 16, prime1Length);
            WriteInt(output, 20, prime2Length);
            int offset = 24;
            foreach (byte[] component in new byte[][] {
                p.Exponent, PadLeft(p.Modulus, modulusLength), PadLeft(p.P, prime1Length),
                PadLeft(p.Q, prime2Length), PadLeft(p.DP, prime1Length), PadLeft(p.DQ, prime2Length),
                PadLeft(p.InverseQ, prime1Length), PadLeft(p.D, modulusLength) }) {
                Buffer.BlockCopy(component, 0, output, offset, component.Length);
                offset += component.Length;
            }
            return output;
        }

        private static void WriteInt(byte[] output, int offset, int value) {
            byte[] bytes = BitConverter.GetBytes(value);
            Buffer.BlockCopy(bytes, 0, output, offset, 4);
        }

        private static string Fingerprint(byte[] publicBlob) {
            using (SHA256 sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(publicBlob)).Replace("-", "");
        }

        public static byte[] LegacyBlobToPkcs8(byte[] legacyBlob) {
            using (RSACryptoServiceProvider rsa = new RSACryptoServiceProvider()) {
                rsa.PersistKeyInCsp = false;
                rsa.ImportCspBlob(legacyBlob);
                if (rsa.KeySize != 3072) throw new CryptographicException("Release key must be RSA-3072.");
                return EncodePkcs8(rsa.ExportParameters(true));
            }
        }

        public static string Pkcs8Fingerprint(byte[] pkcs8) {
            return Fingerprint(PublicBlob(DecodePkcs8(pkcs8)));
        }

        public static byte[] Pkcs8PublicBlob(byte[] pkcs8) {
            return PublicBlob(DecodePkcs8(pkcs8));
        }

        public static byte[] SignPkcs8(byte[] pkcs8, byte[] hash) {
            RSAParameters parameters = DecodePkcs8(pkcs8);
            using (RSACryptoServiceProvider rsa = new RSACryptoServiceProvider()) {
                rsa.PersistKeyInCsp = false;
                rsa.ImportParameters(parameters);
                byte[] signature = rsa.SignHash(hash, CryptoConfig.MapNameToOID("SHA256"));
                if (!rsa.VerifyHash(hash, CryptoConfig.MapNameToOID("SHA256"), signature))
                    throw new CryptographicException("Recovery key challenge verification failed.");
                return signature;
            }
        }

        public static bool VerifyWithBCrypt(byte[] publicBlob, byte[] hash, byte[] signature) {
            IntPtr algorithm = IntPtr.Zero, key = IntPtr.Zero, algorithmName = IntPtr.Zero;
            try {
                Check(BCryptOpenAlgorithmProvider(out algorithm, "RSA", null, 0), "Open BCrypt RSA provider");
                Check(BCryptImportKeyPair(algorithm, IntPtr.Zero, PublicBlobType, out key,
                    publicBlob, publicBlob.Length, 0), "Import BCrypt public key");
                algorithmName = Marshal.StringToHGlobalUni("SHA256");
                Pkcs1PaddingInfo padding = new Pkcs1PaddingInfo { pszAlgId = algorithmName };
                return BCryptVerifySignature(key, ref padding, hash, hash.Length,
                    signature, signature.Length, PadPkcs1) == 0;
            } finally {
                if (algorithmName != IntPtr.Zero) Marshal.FreeHGlobal(algorithmName);
                if (key != IntPtr.Zero) BCryptDestroyKey(key);
                if (algorithm != IntPtr.Zero) BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        }

        public static bool KeyExists(string keyName) {
            IntPtr provider = IntPtr.Zero, key = IntPtr.Zero;
            try {
                Check(NCryptOpenStorageProvider(out provider, ProviderName, 0), "Open Software KSP");
                return NCryptOpenKey(provider, out key, keyName, 0, 0) == 0;
            } finally {
                if (key != IntPtr.Zero) NCryptFreeObject(key);
                if (provider != IntPtr.Zero) NCryptFreeObject(provider);
            }
        }

        public static void ImportSoftwareKey(byte[] pkcs8, string keyName, bool highProtection) {
            if (String.IsNullOrWhiteSpace(keyName)) throw new ArgumentException("Key name is required.");
            IntPtr provider = IntPtr.Zero, key = IntPtr.Zero;
            IntPtr title = IntPtr.Zero, friendly = IntPtr.Zero, description = IntPtr.Zero, policyMemory = IntPtr.Zero;
            bool finalized = false;
            try {
                Check(NCryptOpenStorageProvider(out provider, ProviderName, 0), "Open Software KSP");
                IntPtr existing;
                if (NCryptOpenKey(provider, out existing, keyName, 0, 0) == 0) {
                    NCryptFreeObject(existing);
                    throw new CryptographicException("A CNG key with this name already exists; refusing to overwrite it.");
                }
                Check(NCryptCreatePersistedKey(provider, out key, "RSA", keyName, 0, 0), "Create persisted RSA key");

                byte[] noExport = BitConverter.GetBytes(0);
                Check(NCryptSetProperty(key, "Export Policy", noExport, noExport.Length, Persist), "Set non-exportable policy");
                if (highProtection) {
                    title = Marshal.StringToHGlobalUni("Files4Me release signing key");
                    friendly = Marshal.StringToHGlobalUni(keyName);
                    description = Marshal.StringToHGlobalUni("Approves a Files4Me update manifest signature.");
                    NCryptUiPolicy policy = new NCryptUiPolicy {
                        dwVersion = 1, dwFlags = UiProtect | UiForceHigh,
                        pszCreationTitle = title, pszFriendlyName = friendly, pszDescription = description
                    };
                    policyMemory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(NCryptUiPolicy)));
                    Marshal.StructureToPtr(policy, policyMemory, false);
                    Check(NCryptSetProperty(key, "UI Policy", policyMemory, Marshal.SizeOf(typeof(NCryptUiPolicy)), Persist),
                        "Set strong-use policy");
                }
                byte[] privateBlob = FullPrivateBlob(DecodePkcs8(pkcs8));
                try {
                    Check(NCryptSetProperty(key, "RSAFULLPRIVATEBLOB", privateBlob, privateBlob.Length, 0),
                        "Attach RSA private key");
                } finally {
                    Array.Clear(privateBlob, 0, privateBlob.Length);
                }
                Check(NCryptFinalizeKey(key, 0), "Finalize persisted key");
                finalized = true;
            } catch {
                if (key != IntPtr.Zero && !finalized) { NCryptDeleteKey(key, 0); key = IntPtr.Zero; }
                throw;
            } finally {
                if (policyMemory != IntPtr.Zero) Marshal.FreeHGlobal(policyMemory);
                if (description != IntPtr.Zero) Marshal.ZeroFreeGlobalAllocUnicode(description);
                if (friendly != IntPtr.Zero) Marshal.ZeroFreeGlobalAllocUnicode(friendly);
                if (title != IntPtr.Zero) Marshal.ZeroFreeGlobalAllocUnicode(title);
                if (key != IntPtr.Zero) NCryptFreeObject(key);
                if (provider != IntPtr.Zero) NCryptFreeObject(provider);
            }
        }

        private static byte[] GetProperty(IntPtr handle, string property) {
            int size;
            int status = NCryptGetProperty(handle, property, null, 0, out size, 0);
            Check(status, "Read " + property);
            byte[] result = new byte[size];
            Check(NCryptGetProperty(handle, property, result, result.Length, out size, 0), "Read " + property);
            if (size != result.Length) Array.Resize(ref result, size);
            return result;
        }

        private static byte[] GetOptionalProperty(IntPtr handle, string property) {
            int size;
            int status = NCryptGetProperty(handle, property, null, 0, out size, 0);
            if (status == unchecked((int)0x80090011)) return new byte[0];
            Check(status, "Read " + property);
            byte[] result = new byte[size];
            Check(NCryptGetProperty(handle, property, result, result.Length, out size, 0), "Read " + property);
            if (size != result.Length) Array.Resize(ref result, size);
            return result;
        }

        private static byte[] ExportPublicBlob(IntPtr key) {
            int size;
            Check(NCryptExportKey(key, IntPtr.Zero, PublicBlobType, IntPtr.Zero, null, 0, out size, 0), "Measure public key");
            byte[] result = new byte[size];
            Check(NCryptExportKey(key, IntPtr.Zero, PublicBlobType, IntPtr.Zero, result, result.Length, out size, 0), "Export public key");
            if (size != result.Length) Array.Resize(ref result, size);
            return result;
        }

        public static ReleaseKeyAudit AuditSoftwareKey(string keyName) {
            IntPtr provider = IntPtr.Zero, key = IntPtr.Zero;
            try {
                Check(NCryptOpenStorageProvider(out provider, ProviderName, 0), "Open Software KSP");
                Check(NCryptOpenKey(provider, out key, keyName, 0, 0), "Open release signing key");
                byte[] algorithm = GetProperty(key, "Algorithm Name");
                byte[] length = GetProperty(key, "Length");
                byte[] exportPolicy = GetProperty(key, "Export Policy");
                byte[] uiPolicy = GetOptionalProperty(key, "UI Policy");
                return new ReleaseKeyAudit {
                    KeyName = keyName,
                    Provider = ProviderName,
                    Algorithm = Encoding.Unicode.GetString(algorithm).TrimEnd('\0'),
                    KeySize = BitConverter.ToInt32(length, 0),
                    ExportPolicy = BitConverter.ToInt32(exportPolicy, 0),
                    UiPolicy = uiPolicy.Length >= 8 ? BitConverter.ToInt32(uiPolicy, 4) : 0,
                    PublicFingerprint = Fingerprint(ExportPublicBlob(key))
                };
            } finally {
                if (key != IntPtr.Zero) NCryptFreeObject(key);
                if (provider != IntPtr.Zero) NCryptFreeObject(provider);
            }
        }

        public static byte[] SignSoftwareKey(string keyName, byte[] hash) {
            if (hash == null || hash.Length != 32) throw new ArgumentException("A SHA-256 digest is required.");
            IntPtr provider = IntPtr.Zero, key = IntPtr.Zero, algorithm = IntPtr.Zero;
            try {
                Check(NCryptOpenStorageProvider(out provider, ProviderName, 0), "Open Software KSP");
                Check(NCryptOpenKey(provider, out key, keyName, 0, 0), "Open release signing key");
                algorithm = Marshal.StringToHGlobalUni("SHA256");
                Pkcs1PaddingInfo padding = new Pkcs1PaddingInfo { pszAlgId = algorithm };
                int size;
                Check(NCryptSignHash(key, ref padding, hash, hash.Length, null, 0, out size, PadPkcs1), "Measure signature");
                byte[] signature = new byte[size];
                Check(NCryptSignHash(key, ref padding, hash, hash.Length, signature, signature.Length, out size, PadPkcs1), "Sign manifest");
                if (size != signature.Length) Array.Resize(ref signature, size);
                Check(NCryptVerifySignature(key, ref padding, hash, hash.Length, signature, signature.Length, PadPkcs1),
                    "Self-verify manifest signature");
                return signature;
            } finally {
                if (algorithm != IntPtr.Zero) Marshal.FreeHGlobal(algorithm);
                if (key != IntPtr.Zero) NCryptFreeObject(key);
                if (provider != IntPtr.Zero) NCryptFreeObject(provider);
            }
        }

        public static void DeleteSoftwareKey(string keyName) {
            IntPtr provider = IntPtr.Zero, key = IntPtr.Zero;
            try {
                Check(NCryptOpenStorageProvider(out provider, ProviderName, 0), "Open Software KSP");
                Check(NCryptOpenKey(provider, out key, keyName, 0, 0), "Open release signing key");
                Check(NCryptDeleteKey(key, 0), "Delete release signing key");
                key = IntPtr.Zero;
            } finally {
                if (key != IntPtr.Zero) NCryptFreeObject(key);
                if (provider != IntPtr.Zero) NCryptFreeObject(provider);
            }
        }
    }
}
'@
}

function Get-Files4MeOpenSsl {
    if ($script:OpenSslPath) { return $script:OpenSslPath }
    $gitOpenSsl = Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'
    $candidates = @()
    if (Test-Path -LiteralPath $gitOpenSsl) { $candidates += $gitOpenSsl }
    $command = Get-Command openssl.exe -ErrorAction SilentlyContinue
    if ($command -and $command.Source -notin $candidates) { $candidates += $command.Source }
    foreach ($candidate in $candidates) {
        $version = & $candidate version 2>$null
        if ($LASTEXITCODE -eq 0 -and $version -match '^OpenSSL 3\.') {
            $script:OpenSslPath = $candidate
            return $candidate
        }
    }
    throw 'OpenSSL 3 was not found. Install Git for Windows or OpenSSL 3 before managing recovery backups.'
}

function ConvertFrom-SecureStringPlaintext {
    param([Parameter(Mandatory=$true)][Security.SecureString]$Value)
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Value)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

function Read-ConfirmedPassphrase {
    $first = Read-Host 'Recovery-backup passphrase' -AsSecureString
    $second = Read-Host 'Confirm recovery-backup passphrase' -AsSecureString
    $firstText = ConvertFrom-SecureStringPlaintext $first
    $secondText = ConvertFrom-SecureStringPlaintext $second
    try {
        if ($firstText.Length -lt 16) { throw 'Use a recovery passphrase of at least 16 characters.' }
        if ($firstText -cne $secondText) { throw 'Recovery passphrases do not match.' }
        return $first
    }
    finally { $firstText = $null; $secondText = $null }
}

function Invoke-OpenSslBytes {
    param(
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][byte[]]$InputBytes,
        [Parameter(Mandatory=$true)][Security.SecureString]$Passphrase
    )
    $plain = ConvertFrom-SecureStringPlaintext $Passphrase
    try {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = Get-Files4MeOpenSsl
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.Arguments = ($Arguments | ForEach-Object {
            if ($_ -match '[\s"]') { '"' + ($_ -replace '"','\"') + '"' } else { $_ }
        }) -join ' '
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $start
        $oldPassword = [Environment]::GetEnvironmentVariable('FILES4ME_PKCS8_PASSWORD', 'Process')
        [Environment]::SetEnvironmentVariable('FILES4ME_PKCS8_PASSWORD', $plain, 'Process')
        try {
            if (-not $process.Start()) { throw 'OpenSSL could not be started.' }
        }
        finally {
            [Environment]::SetEnvironmentVariable('FILES4ME_PKCS8_PASSWORD', $oldPassword, 'Process')
        }
        try {
            $process.StandardInput.BaseStream.Write($InputBytes, 0, $InputBytes.Length)
            $process.StandardInput.Close()
            $output = [IO.MemoryStream]::new()
            try {
                $process.StandardOutput.BaseStream.CopyTo($output)
                $errorText = $process.StandardError.ReadToEnd()
                $process.WaitForExit()
                if ($process.ExitCode -ne 0) { throw "OpenSSL failed: $($errorText.Trim())" }
                return $output.ToArray()
            }
            finally { $output.Dispose() }
        }
        finally { $process.Dispose() }
    }
    finally { $plain = $null }
}

function Protect-Files4MePkcs8 {
    param([byte[]]$Pkcs8, [Security.SecureString]$Passphrase)
    Invoke-OpenSslBytes -InputBytes $Pkcs8 -Passphrase $Passphrase -Arguments @(
        'pkcs8','-topk8','-inform','DER','-outform','PEM','-v2','aes-256-cbc',
        '-v2prf','hmacWithSHA256','-iter',"$script:Pkcs8Iterations",'-passout','env:FILES4ME_PKCS8_PASSWORD'
    )
}

function Unprotect-Files4MePkcs8 {
    param([byte[]]$EncryptedPem, [Security.SecureString]$Passphrase)
    Invoke-OpenSslBytes -InputBytes $EncryptedPem -Passphrase $Passphrase -Arguments @(
        'pkey','-inform','PEM','-outform','DER','-passin','env:FILES4ME_PKCS8_PASSWORD'
    )
}

function Assert-ExpectedFingerprint {
    param([Parameter(Mandatory=$true)][string]$Fingerprint)
    if ($Fingerprint -cne $script:ExpectedPublicFingerprint) {
        throw "Signing-key fingerprint mismatch. Expected $script:ExpectedPublicFingerprint but found $Fingerprint."
    }
}

function Test-Files4MeRecoveryBytes {
    param([byte[]]$Pkcs8)
    $fingerprint = [Files4Me.ReleaseKeyNative]::Pkcs8Fingerprint($Pkcs8)
    Assert-ExpectedFingerprint $fingerprint
    $challenge = [Text.Encoding]::UTF8.GetBytes('Files4Me release-key recovery challenge v1')
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($challenge) } finally { $sha.Dispose() }
    $signature = [Files4Me.ReleaseKeyNative]::SignPkcs8($Pkcs8, $digest)
    if ($signature.Length -ne 384) { throw 'Recovery key produced an unexpected signature length.' }
    [Array]::Clear($signature, 0, $signature.Length)
    return $fingerprint
}

function Get-Files4MeReleaseKeyAudit {
    param([string]$KeyName = $script:DefaultKeyName, [switch]$AllowTestPolicy)
    $audit = [Files4Me.ReleaseKeyNative]::AuditSoftwareKey($KeyName)
    if ($audit.Provider -cne $script:SoftwareProvider) { throw 'Release key is not stored in the Microsoft Software KSP.' }
    if ($audit.Algorithm -cne 'RSA' -or $audit.KeySize -ne 3072) { throw 'Release key is not RSA-3072.' }
    if (-not $audit.NonExportable) { throw 'Release key is exportable.' }
    if (-not $AllowTestPolicy -and -not $audit.HighProtection) { throw 'Release key does not require strong-use confirmation.' }
    Assert-ExpectedFingerprint $audit.PublicFingerprint
    return $audit
}

function Invoke-Files4MeReleaseSignature {
    param([Parameter(Mandatory=$true)][byte[]]$Digest, [string]$KeyName = $script:DefaultKeyName)
    Get-Files4MeReleaseKeyAudit -KeyName $KeyName | Out-Null
    [Files4Me.ReleaseKeyNative]::SignSoftwareKey($KeyName, $Digest)
}

function New-Files4MeRecoveryBackups {
    param(
        [Parameter(Mandatory=$true)][byte[]]$Pkcs8,
        [Parameter(Mandatory=$true)][string[]]$BackupPath,
        [Parameter(Mandatory=$true)][Security.SecureString]$Passphrase
    )
    if ($BackupPath.Count -ne 2) { throw 'Exactly two recovery-backup paths are required.' }
    $fullPaths = @($BackupPath | ForEach-Object { [IO.Path]::GetFullPath($_) })
    if ($fullPaths[0] -ieq $fullPaths[1]) { throw 'Recovery-backup paths must be different.' }
    $roots = @($fullPaths | ForEach-Object { [IO.Path]::GetPathRoot($_) })
    if ($roots[0] -ieq $roots[1]) { throw 'Recovery backups must be stored on different volumes.' }
    $repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    foreach ($path in $fullPaths) {
        if ($path.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Recovery backups must not be stored inside the Files4Me repository.'
        }
        $parent = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Backup directory does not exist: $parent" }
        if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite recovery backup: $path" }
        $encrypted = Protect-Files4MePkcs8 -Pkcs8 $Pkcs8 -Passphrase $Passphrase
        try {
            $stream = [IO.FileStream]::new($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $stream.Write($encrypted, 0, $encrypted.Length); $stream.Flush($true) }
            finally { $stream.Dispose() }
        }
        finally { [Array]::Clear($encrypted, 0, $encrypted.Length) }
    }
    foreach ($path in $fullPaths) {
        $encrypted = [IO.File]::ReadAllBytes($path)
        $restored = Unprotect-Files4MePkcs8 -EncryptedPem $encrypted -Passphrase $Passphrase
        try { Test-Files4MeRecoveryBytes $restored | Out-Null }
        finally {
            [Array]::Clear($encrypted, 0, $encrypted.Length)
            [Array]::Clear($restored, 0, $restored.Length)
        }
    }
    return $fullPaths
}

function Import-Files4MeLegacyReleaseKey {
    param(
        [Parameter(Mandatory=$true)][string]$LegacyKey,
        [Parameter(Mandatory=$true)][string[]]$BackupPath,
        [string]$KeyName = $script:DefaultKeyName,
        [Security.SecureString]$Passphrase
    )
    if ([Files4Me.ReleaseKeyNative]::KeyExists($KeyName)) { throw 'The destination CNG key already exists; run Audit instead.' }
    $legacyPath = (Resolve-Path -LiteralPath $LegacyKey).Path
    $legacyBytes = [IO.File]::ReadAllBytes($legacyPath)
    $pkcs8 = $null
    try {
        $pkcs8 = [Files4Me.ReleaseKeyNative]::LegacyBlobToPkcs8($legacyBytes)
        Assert-ExpectedFingerprint ([Files4Me.ReleaseKeyNative]::Pkcs8Fingerprint($pkcs8))
        if (-not $Passphrase) { $Passphrase = Read-ConfirmedPassphrase }
        $backups = New-Files4MeRecoveryBackups -Pkcs8 $pkcs8 -BackupPath $BackupPath -Passphrase $Passphrase
        [Files4Me.ReleaseKeyNative]::ImportSoftwareKey($pkcs8, $KeyName, $true)
        $audit = Get-Files4MeReleaseKeyAudit -KeyName $KeyName
        [pscustomobject]@{
            Key = $audit
            RecoveryBackups = $backups
            LegacyKeyRetained = $legacyPath
            Message = 'Migration verified. The legacy key was intentionally not deleted.'
        }
    }
    finally {
        [Array]::Clear($legacyBytes, 0, $legacyBytes.Length)
        if ($pkcs8) { [Array]::Clear($pkcs8, 0, $pkcs8.Length) }
    }
}

function Test-Files4MeRecoveryBackup {
    param(
        [Parameter(Mandatory=$true)][string[]]$BackupPath,
        [Security.SecureString]$Passphrase
    )
    if (-not $Passphrase) { $Passphrase = Read-Host 'Recovery-backup passphrase' -AsSecureString }
    foreach ($path in $BackupPath) {
        $resolved = (Resolve-Path -LiteralPath $path).Path
        $encrypted = [IO.File]::ReadAllBytes($resolved)
        $pkcs8 = Unprotect-Files4MePkcs8 -EncryptedPem $encrypted -Passphrase $Passphrase
        try {
            [pscustomobject]@{ Backup = $resolved; PublicFingerprint = (Test-Files4MeRecoveryBytes $pkcs8); Verified = $true }
        }
        finally {
            [Array]::Clear($encrypted, 0, $encrypted.Length)
            [Array]::Clear($pkcs8, 0, $pkcs8.Length)
        }
    }
}

function Restore-Files4MeReleaseKey {
    param(
        [Parameter(Mandatory=$true)][string]$BackupPath,
        [string]$KeyName = $script:DefaultKeyName,
        [Security.SecureString]$Passphrase
    )
    if ([Files4Me.ReleaseKeyNative]::KeyExists($KeyName)) { throw 'The destination CNG key already exists; refusing to overwrite it.' }
    if (-not $Passphrase) { $Passphrase = Read-Host 'Recovery-backup passphrase' -AsSecureString }
    $encrypted = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $BackupPath).Path)
    $pkcs8 = Unprotect-Files4MePkcs8 -EncryptedPem $encrypted -Passphrase $Passphrase
    try {
        Test-Files4MeRecoveryBytes $pkcs8 | Out-Null
        [Files4Me.ReleaseKeyNative]::ImportSoftwareKey($pkcs8, $KeyName, $true)
        Get-Files4MeReleaseKeyAudit -KeyName $KeyName
    }
    finally {
        [Array]::Clear($encrypted, 0, $encrypted.Length)
        [Array]::Clear($pkcs8, 0, $pkcs8.Length)
    }
}

Export-ModuleMember -Function Get-Files4MeReleaseKeyAudit,Invoke-Files4MeReleaseSignature,
    Import-Files4MeLegacyReleaseKey,Test-Files4MeRecoveryBackup,Restore-Files4MeReleaseKey
