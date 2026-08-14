#include "update.h"

#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kMaximumInstallerSize = 256ULL * 1024 * 1024;
constexpr DWORD kMaximumManifestSize = 16 * 1024;
constexpr wchar_t kSignatureSuffix[] = L".sig";

// RSA-3072 public key. Its private half is stored outside the repository.
constexpr BYTE kUpdatePublicKey[] = {
    0x52,0x53,0x41,0x31,0x00,0x0C,0x00,0x00,0x03,0x00,0x00,0x00,0x80,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0xE3,0x53,0x1E,0x48,0x6E,
    0x2C,0x40,0xBC,0x5C,0xFF,0x15,0x6D,0x88,0x9B,0x0E,0xA3,0x84,0x15,0x97,0x39,0xDE,
    0x6C,0x82,0xBF,0x4D,0x83,0xD9,0x13,0x68,0xE8,0xA4,0x33,0xEE,0x14,0x08,0xB6,0xCD,
    0x26,0xDD,0x45,0xFB,0x5A,0x1B,0xBD,0x52,0x4B,0x77,0xAC,0x51,0x2B,0x2A,0xC0,0x10,
    0x40,0x7A,0x7C,0x3D,0x2B,0x90,0x9C,0x04,0x09,0x52,0xBF,0xCE,0x01,0x5C,0x1C,0x04,
    0x75,0x18,0x8E,0x72,0x85,0x35,0x6C,0x8F,0x96,0x51,0x61,0xBB,0xB9,0x57,0xCC,0xDE,
    0x75,0xAB,0x19,0x22,0x32,0xF6,0x3A,0xC5,0x4B,0xEE,0xD8,0x96,0x65,0x59,0x79,0x98,
    0x67,0x26,0xBA,0xD7,0x79,0xB3,0x04,0x1F,0xEF,0xAB,0xBD,0xFC,0xCA,0xDE,0x0B,0x56,
    0xA6,0xD9,0xF1,0x4C,0x79,0x22,0x79,0xBF,0x4A,0xD8,0x57,0xD0,0xD2,0x09,0xE1,0xBC,
    0x93,0x75,0x08,0xB2,0x6F,0x5D,0xFD,0x6E,0x9C,0xD6,0x36,0xE5,0xB4,0x5A,0x1F,0x19,
    0x6B,0x59,0x53,0x21,0xA4,0x25,0xFF,0x6D,0xA7,0x1C,0x24,0xC1,0x13,0x15,0xBB,0xDA,
    0xAD,0xD9,0x53,0xA8,0xC9,0x53,0x88,0x93,0x4B,0xE5,0x82,0xA2,0xA0,0x56,0xCD,0x87,
    0xB8,0x03,0x53,0x03,0x88,0x87,0xDE,0x3A,0x50,0x3C,0x13,0xC5,0xCD,0xFF,0x76,0xD4,
    0x5F,0xBD,0x7A,0x0A,0x6E,0xA4,0x51,0xB7,0x32,0xB0,0x21,0x4D,0x4C,0xE6,0x61,0x72,
    0x84,0xC1,0xE4,0xF3,0x3A,0xFB,0x31,0xF3,0x04,0x0F,0x5F,0xB8,0xB0,0x14,0x7D,0x18,
    0x83,0x20,0xD4,0x32,0x97,0x6A,0xE5,0xA9,0x0E,0xDF,0x2E,0x20,0x0A,0x10,0x43,0x12,
    0x7D,0x3C,0xF0,0x8A,0x44,0x12,0x47,0xB8,0xD8,0x44,0x77,0xBE,0x5F,0x60,0x7A,0xCA,
    0x41,0xA3,0x24,0x61,0x95,0xD1,0x30,0xC4,0x1E,0x72,0x75,0xA3,0x5B,0x71,0xA2,0xEE,
    0xAE,0x84,0xB6,0x6B,0xEF,0x33,0xC6,0x1A,0x4E,0x2E,0x62,0x4B,0x49,0xAC,0xA7,0x11,
    0x98,0x20,0x08,0x25,0x9F,0x0F,0xCF,0x85,0xCD,0x9E,0x50,0x33,0xD0,0x84,0x7C,0x16,
    0x4C,0x9A,0x64,0x71,0xC5,0xB5,0x9C,0xAC,0xE8,0x79,0xF1,0x45,0x35,0x79,0xF6,0x5A,
    0xB0,0x54,0x5C,0x57,0xBA,0x67,0x78,0x4E,0x9D,0x82,0x13,0x5F,0x45,0xB4,0x3B,0x0F,
    0x04,0x6F,0x95,0x07,0x59,0xE1,0xC0,0x49,0x9F,0x8B,0x86,0x69,0x6E,0x84,0xAF,0x86,
    0x21,0xAF,0x22,0x5C,0x2D,0xDD,0xDA,0xD4,0xA7,0x8A,0x8D,0x8B,0x22,0x84,0x99,0x88,
    0x43,0x3C,0x73,0xB2,0x46,0xB0,0x60,0xE4,0xFB,0xEB,0xE5
};

struct InternetCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using InternetHandle = std::unique_ptr<void, InternetCloser>;

bool AllowedHost(const std::wstring& host) {
    constexpr wchar_t githubContentSuffix[] = L".githubusercontent.com";
    constexpr size_t suffixLength = ARRAYSIZE(githubContentSuffix) - 1;
    return _wcsicmp(host.c_str(), L"raw.githubusercontent.com") == 0 ||
           _wcsicmp(host.c_str(), L"github.com") == 0 ||
           _wcsicmp(host.c_str(), L"objects.githubusercontent.com") == 0 ||
           (host.size() > suffixLength &&
            _wcsicmp(host.c_str() + host.size() - suffixLength, githubContentSuffix) == 0);
}

bool ParseHttpsUrl(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port) {
    URL_COMPONENTSW parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1); parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    port = parts.nPort;
    return AllowedHost(host) && !path.empty();
}

bool HttpGet(const std::wstring& initialUrl, DWORD maximumBytes, std::vector<BYTE>& output, std::wstring& detail) {
    InternetHandle session(WinHttpOpen(L"Files4Me/0.3-alpha update checker", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { detail = L"WinHTTP initialization failed"; return false; }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
    std::wstring url = initialUrl;
    for (int redirect = 0; redirect <= 5; ++redirect) {
        std::wstring host, path; INTERNET_PORT port = 0;
        if (!ParseHttpsUrl(url, host, path, port)) { detail = L"Update URL is not trusted"; return false; }
        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
        if (!connection) { detail = L"Connection failed"; return false; }
        InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE));
        if (!request) { detail = L"Request creation failed"; return false; }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) { detail = L"Network request failed"; return false; }
        DWORD status = 0, statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
            detail = L"Invalid HTTP response"; return false;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            DWORD locationSize = 0;
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &locationSize, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locationSize > 8192) { detail = L"Invalid redirect"; return false; }
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &locationSize, WINHTTP_NO_HEADER_INDEX)) return false;
            location.resize(wcslen(location.c_str())); url = std::move(location); continue;
        }
        if (status != 200) { detail = L"Update server returned HTTP " + std::to_wstring(status); return false; }
        output.clear();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) { detail = L"Response read failed"; return false; }
            if (!available) return true;
            if (available > maximumBytes || output.size() > maximumBytes - available) { detail = L"Response exceeds size limit"; return false; }
            const size_t oldSize = output.size(); output.resize(oldSize + available); DWORD read = 0;
            if (!WinHttpReadData(request.get(), output.data() + oldSize, available, &read)) { detail = L"Response read failed"; return false; }
            output.resize(oldSize + read);
        }
    }
    detail = L"Too many redirects"; return false;
}

bool Sha256(const BYTE* data, size_t size, std::array<BYTE, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    const auto closeAlgorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0); };
    DWORD objectSize = 0, returned = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0) { closeAlgorithm(); return false; }
    std::vector<BYTE> object(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) { closeAlgorithm(); return false; }
    const NTSTATUS hashed = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
    const NTSTATUS finished = hashed < 0 ? hashed : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash); closeAlgorithm(); return finished >= 0;
}

std::vector<BYTE> Base64Decode(const std::vector<BYTE>& text) {
    static constexpr signed char table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    std::vector<BYTE> output; unsigned accumulator = 0; int bits = 0;
    for (BYTE character : text) {
        if (character >= 128) return {};
        const int value = table[character];
        if (value == -1) { if (iswspace(character)) continue; return {}; }
        if (value == -2) break;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value); bits += 6;
        if (bits >= 8) { bits -= 8; output.push_back(static_cast<BYTE>((accumulator >> bits) & 0xff)); }
    }
    return output;
}

bool VerifyManifest(const std::vector<BYTE>& manifest, const std::vector<BYTE>& signatureText) {
    const std::vector<BYTE> signature = Base64Decode(signatureText);
    if (signature.size() != 384) return false;
    std::array<BYTE, 32> digest{}; if (!Sha256(manifest.data(), manifest.size(), digest)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                            const_cast<PUCHAR>(kUpdatePublicKey), sizeof(kUpdatePublicKey), 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); return false;
    }
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    const NTSTATUS status = BCryptVerifySignature(key, &padding, digest.data(), static_cast<ULONG>(digest.size()),
                                                   const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()),
                                                   BCRYPT_PAD_PKCS1);
    BCryptDestroyKey(key); BCryptCloseAlgorithmProvider(algorithm, 0); return status >= 0;
}

std::wstring Utf8(const std::vector<BYTE>& bytes) {
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                               static_cast<int>(bytes.size()), value.data(), length) ? value : std::wstring{};
}

bool ParseManifest(const std::vector<BYTE>& bytes, UpdateManifest& manifest) {
    const std::wstring text = Utf8(bytes); if (text.empty()) return false;
    std::wstring schema; size_t position = 0;
    while (position <= text.size()) {
        const size_t end = text.find(L'\n', position); std::wstring line = text.substr(position, end - position);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line.front() != L'#') {
            const size_t equals = line.find(L'='); if (equals == std::wstring::npos) return false;
            const std::wstring key = line.substr(0, equals), value = line.substr(equals + 1);
            if (key == L"schema") schema = value; else if (key == L"version") manifest.version = value;
            else if (key == L"display_version") manifest.displayVersion = value;
            else if (key == L"installer_url") manifest.installerUrl = value;
            else if (key == L"release_url") manifest.releaseUrl = value;
            else if (key == L"sha256") manifest.sha256 = value; else if (key == L"notes") manifest.notes = value;
            else if (key == L"size") {
                wchar_t* numericEnd = nullptr; errno = 0;
                manifest.size = _wcstoui64(value.c_str(), &numericEnd, 10);
                if (errno == ERANGE || numericEnd != value.c_str() + value.size()) return false;
            } else return false;
        }
        if (end == std::wstring::npos) break; position = end + 1;
    }
    std::wstring host, path; INTERNET_PORT port = 0;
    return schema == L"1" && !manifest.version.empty() && !manifest.displayVersion.empty() &&
           manifest.sha256.size() == 64 && manifest.size > 0 && manifest.size <= kMaximumInstallerSize &&
           ParseHttpsUrl(manifest.installerUrl, host, path, port) && ParseHttpsUrl(manifest.releaseUrl, host, path, port);
}

struct Version { std::array<unsigned,3> numbers{}; std::vector<std::wstring> prerelease; bool valid = false; };
Version ParseVersion(const std::wstring& text) {
    Version version; const size_t dash = text.find(L'-'); const std::wstring core = text.substr(0, dash);
    size_t start = 0;
    for (size_t index = 0; index < 3; ++index) {
        const size_t dot = core.find(L'.', start); const std::wstring part = core.substr(start, dot - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(), iswdigit)) return version;
        unsigned long number = wcstoul(part.c_str(), nullptr, 10); if (number > UINT_MAX) return version;
        version.numbers[index] = static_cast<unsigned>(number);
        if (index < 2 && dot == std::wstring::npos) return version; start = dot + 1;
    }
    if (dash != std::wstring::npos) {
        start = dash + 1;
        while (start <= text.size()) { const size_t dot = text.find(L'.', start); const std::wstring part = text.substr(start, dot - start); if (part.empty()) return version; version.prerelease.push_back(part); if (dot == std::wstring::npos) break; start = dot + 1; }
    }
    version.valid = true; return version;
}
int CompareVersions(const std::wstring& leftText, const std::wstring& rightText) {
    const Version left = ParseVersion(leftText), right = ParseVersion(rightText); if (!left.valid || !right.valid) return 0;
    if (left.numbers != right.numbers) return left.numbers < right.numbers ? -1 : 1;
    if (left.prerelease.empty() != right.prerelease.empty()) return left.prerelease.empty() ? 1 : -1;
    for (size_t i = 0; i < (std::min)(left.prerelease.size(), right.prerelease.size()); ++i) {
        const bool leftNumber = std::all_of(left.prerelease[i].begin(), left.prerelease[i].end(), iswdigit);
        const bool rightNumber = std::all_of(right.prerelease[i].begin(), right.prerelease[i].end(), iswdigit);
        if (leftNumber && rightNumber) { const auto l = wcstoull(left.prerelease[i].c_str(), nullptr, 10), r = wcstoull(right.prerelease[i].c_str(), nullptr, 10); if (l != r) return l < r ? -1 : 1; }
        else if (leftNumber != rightNumber) return leftNumber ? -1 : 1;
        else { const int compared = _wcsicmp(left.prerelease[i].c_str(), right.prerelease[i].c_str()); if (compared) return compared < 0 ? -1 : 1; }
    }
    return left.prerelease.size() == right.prerelease.size() ? 0 : (left.prerelease.size() < right.prerelease.size() ? -1 : 1);
}

std::wstring HexDigest(const std::array<BYTE,32>& digest) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF"; std::wstring text(64, L'0');
    for (size_t i = 0; i < digest.size(); ++i) { text[i*2] = digits[digest[i] >> 4]; text[i*2+1] = digits[digest[i] & 15]; }
    return text;
}

void PostResult(HWND target, UINT message, std::unique_ptr<UpdateResult> result) {
    UpdateResult* raw = result.release(); if (!PostMessageW(target, message, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
}

} // namespace

void BeginUpdateCheck(HWND target, UINT completionMessage, bool manual) {
    std::thread([target, completionMessage, manual] {
        auto result = std::make_unique<UpdateResult>(); result->manual = manual;
        std::vector<BYTE> manifestBytes, signatureBytes;
        if (!HttpGet(Files4MeUpdateManifestUrl, kMaximumManifestSize, manifestBytes, result->detail) ||
            !HttpGet(std::wstring(Files4MeUpdateManifestUrl) + kSignatureSuffix, 2048, signatureBytes, result->detail)) {
            result->status = UpdateStatus::NetworkError; PostResult(target, completionMessage, std::move(result)); return;
        }
        if (!VerifyManifest(manifestBytes, signatureBytes)) {
            result->status = UpdateStatus::VerificationFailed; result->detail = L"Update manifest signature is invalid";
            PostResult(target, completionMessage, std::move(result)); return;
        }
        if (!ParseManifest(manifestBytes, result->manifest) || !ParseVersion(result->manifest.version).valid) {
            result->status = UpdateStatus::InvalidManifest; result->detail = L"Update manifest is invalid";
        } else result->status = CompareVersions(result->manifest.version, Files4MeCurrentVersion) > 0 ?
                                UpdateStatus::Available : UpdateStatus::UpToDate;
        PostResult(target, completionMessage, std::move(result));
    }).detach();
}

void BeginUpdateDownload(HWND target, UINT completionMessage, const UpdateManifest& manifest,
                         const std::wstring& stateDirectory) {
    std::thread([target, completionMessage, manifest, stateDirectory] {
        auto result = std::make_unique<UpdateResult>(); result->manual = true; result->manifest = manifest;
        std::vector<BYTE> bytes;
        if (!HttpGet(manifest.installerUrl, static_cast<DWORD>(manifest.size), bytes, result->detail) || bytes.size() != manifest.size) {
            result->status = UpdateStatus::NetworkError; PostResult(target, completionMessage, std::move(result)); return;
        }
        std::array<BYTE,32> digest{};
        if (!Sha256(bytes.data(), bytes.size(), digest) || _wcsicmp(HexDigest(digest).c_str(), manifest.sha256.c_str()) != 0) {
            result->status = UpdateStatus::VerificationFailed; result->detail = L"Downloaded installer hash does not match";
            PostResult(target, completionMessage, std::move(result)); return;
        }
        const std::wstring directory = stateDirectory + L"\\Updates"; CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring finalPath = directory + L"\\Files4Me-" + manifest.displayVersion + L"-Setup.exe";
        const std::wstring temporary = finalPath + L".part";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { result->status = UpdateStatus::NetworkError; result->detail = L"Cannot create update file"; PostResult(target, completionMessage, std::move(result)); return; }
        DWORD written = 0; const BOOL okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && FlushFileBuffers(file); CloseHandle(file);
        if (!okay || written != bytes.size() || !MoveFileExW(temporary.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); result->status = UpdateStatus::NetworkError; result->detail = L"Cannot save update file";
        } else { result->status = UpdateStatus::DownloadReady; result->downloadedPath = finalPath; }
        PostResult(target, completionMessage, std::move(result));
    }).detach();
}

bool IsFiles4MeInstalledBuild() {
    wchar_t executable[32768]{}; if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
    std::wstring path = executable; const size_t slash = path.find_last_of(L"\\/"); if (slash != std::wstring::npos) path.resize(slash + 1);
    return GetFileAttributesW((path + L"installed.marker").c_str()) != INVALID_FILE_ATTRIBUTES;
}
