#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include "version.h"

enum class UpdateStatus { UpToDate, Available, DownloadReady, NetworkError, InvalidManifest, VerificationFailed };

struct UpdateManifest {
    std::wstring version;
    std::wstring displayVersion;
    std::wstring installerUrl;
    std::wstring releaseUrl;
    std::wstring sha256;
    std::wstring notes;
    uint64_t size = 0;
};

struct UpdateResult {
    UpdateStatus status = UpdateStatus::NetworkError;
    UpdateManifest manifest;
    std::wstring detail;
    std::wstring downloadedPath;
    bool manual = false;
};

constexpr wchar_t Files4MeCurrentVersion[] = FILES4ME_VERSION_SEMVER_W;
constexpr wchar_t Files4MeUpdateManifestUrl[] =
    L"https://raw.githubusercontent.com/adamng04/Files4Me/main/updates/alpha/manifest.ini";

void BeginUpdateCheck(HWND target, UINT completionMessage, bool manual);
void BeginUpdateDownload(HWND target, UINT completionMessage, const UpdateManifest& manifest,
                         const std::wstring& stateDirectory);
bool IsFiles4MeInstalledBuild();
