#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct ArchiveEntryData {
    std::wstring name;
    std::wstring internalPath;
    std::wstring extension;
    std::wstring type;
    ULONGLONG size = 0;
    FILETIME modified{};
    DWORD attributes = 0;
    int icon = 0;
};

HRESULT EnumerateZipFolder(const std::wstring& archivePath, const std::wstring& internalPath,
                           std::vector<ArchiveEntryData>& entries);
HRESULT ExtractZipEntries(const std::wstring& archivePath, const std::vector<std::wstring>& internalPaths,
                          const std::wstring& destination);
