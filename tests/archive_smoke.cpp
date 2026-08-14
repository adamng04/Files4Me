#include "../src/archive.h"

#include <objbase.h>
#include <shlobj.h>
#include <windows.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 3;
    std::vector<ArchiveEntryData> entries;
    HRESULT result = EnumerateZipFolder(argv[1], L"", entries);
    if (FAILED(result) || entries.empty()) {
        std::wcerr << L"enumeration failed: 0x" << std::hex << static_cast<unsigned long>(result)
                   << L", items=" << std::dec << entries.size() << L"\n";
        CoUninitialize(); return 4;
    }
    SHCreateDirectoryExW(nullptr, argv[2], nullptr);
    result = ExtractZipEntries(argv[1], {entries.front().internalPath}, argv[2]);
    const std::wstring extracted = std::wstring(argv[2]) + L"\\" + entries.front().name;
    const bool exists = GetFileAttributesW(extracted.c_str()) != INVALID_FILE_ATTRIBUTES;
    CoUninitialize();
    if (FAILED(result) || !exists) return 5;
    std::wcout << L"archive smoke passed: " << entries.size() << L" root item(s)\n";
    return 0;
}
