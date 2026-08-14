#include "archive.h"

#include <propsys.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring JoinInternal(const std::wstring& base, const std::wstring& name) {
    return base.empty() ? name : base + L"\\" + name;
}

HRESULT BindZipRoot(const std::wstring& archivePath, ComPtr<IShellFolder>& root) {
    ComPtr<IShellItem> archive;
    HRESULT result = SHCreateItemFromParsingName(archivePath.c_str(), nullptr, IID_PPV_ARGS(&archive));
    if (FAILED(result)) return result;
    return archive->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&root));
}

HRESULT ParseRelative(IShellFolder* root, const std::wstring& internalPath,
                      PIDLIST_RELATIVE* relative) {
    if (!root || !relative || internalPath.empty()) return E_INVALIDARG;
    *relative = nullptr;
    std::vector<wchar_t> mutableName(internalPath.begin(), internalPath.end());
    mutableName.push_back(L'\0');
    ULONG eaten = 0;
    SFGAOF attributes = 0;
    return root->ParseDisplayName(nullptr, nullptr, mutableName.data(), &eaten, relative, &attributes);
}

HRESULT BindZipFolder(IShellFolder* root, const std::wstring& internalPath,
                      ComPtr<IShellFolder>& folder) {
    if (internalPath.empty()) {
        folder = root;
        return S_OK;
    }
    PIDLIST_RELATIVE relative = nullptr;
    HRESULT result = ParseRelative(root, internalPath, &relative);
    if (SUCCEEDED(result)) result = root->BindToObject(relative, nullptr, IID_PPV_ARGS(&folder));
    CoTaskMemFree(relative);
    return result;
}

std::wstring ShellString(IShellItem2* item, REFPROPERTYKEY key) {
    PWSTR value = nullptr;
    if (!item || FAILED(item->GetString(key, &value)) || !value) return {};
    std::wstring output(value);
    CoTaskMemFree(value);
    return output;
}

int IconForEntry(const std::wstring& name, DWORD attributes) {
    SHFILEINFOW info{};
    return SHGetFileInfoW(name.c_str(), attributes, &info, sizeof(info),
                          SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)
        ? info.iIcon : 0;
}

} // namespace

HRESULT EnumerateZipFolder(const std::wstring& archivePath, const std::wstring& internalPath,
                           std::vector<ArchiveEntryData>& entries) {
    entries.clear();
    ComPtr<IShellFolder> root;
    HRESULT result = BindZipRoot(archivePath, root);
    if (FAILED(result)) return result;
    ComPtr<IShellFolder> folder;
    result = BindZipFolder(root.Get(), internalPath, folder);
    if (FAILED(result)) return result;

    ComPtr<IEnumIDList> enumerator;
    result = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                                 &enumerator);
    if (result == S_FALSE) return S_OK;
    if (FAILED(result)) return result;

    PIDLIST_RELATIVE child = nullptr;
    while (enumerator->Next(1, &child, nullptr) == S_OK) {
        ArchiveEntryData entry;
        STRRET display{};
        wchar_t name[32768]{};
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &display)) &&
            SUCCEEDED(StrRetToBufW(&display, child, name, ARRAYSIZE(name)))) {
            entry.name = name;
        }
        SFGAOF shellAttributes = SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_FILESYSTEM;
        PCUITEMID_CHILD children[] = {child};
        folder->GetAttributesOf(1, children, &shellAttributes);
        entry.attributes = (shellAttributes & SFGAO_FOLDER) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (shellAttributes & SFGAO_HIDDEN) entry.attributes |= FILE_ATTRIBUTE_HIDDEN;

        ComPtr<IShellItem2> item;
        if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder.Get(), child, IID_PPV_ARGS(&item)))) {
            std::wstring propertyName = ShellString(item.Get(), PKEY_ItemNameDisplay);
            if (!propertyName.empty()) entry.name = std::move(propertyName);
            item->GetUInt64(PKEY_Size, &entry.size);
            item->GetFileTime(PKEY_DateModified, &entry.modified);
            entry.type = ShellString(item.Get(), PKEY_ItemTypeText);
        }
        if (entry.name.empty()) entry.name = L"Archive item";
        entry.internalPath = JoinInternal(internalPath, entry.name);
        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const wchar_t* extension = PathFindExtensionW(entry.name.c_str());
            if (extension && *extension && extension != entry.name.c_str()) entry.extension = extension + 1;
        }
        if (entry.type.empty()) entry.type = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY)
            ? L"Folder" : (entry.extension.empty() ? L"File" : entry.extension + L" file");
        entry.icon = IconForEntry(entry.name, entry.attributes);
        entries.push_back(std::move(entry));
        CoTaskMemFree(child);
        child = nullptr;
    }
    CoTaskMemFree(child);
    return S_OK;
}

HRESULT ExtractZipEntries(const std::wstring& archivePath, const std::vector<std::wstring>& internalPaths,
                          const std::wstring& destination) {
    if (internalPaths.empty()) return S_FALSE;
    ComPtr<IShellFolder> root;
    HRESULT result = BindZipRoot(archivePath, root);
    if (FAILED(result)) return result;

    ComPtr<IShellItem> destinationItem;
    result = SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&destinationItem));
    if (FAILED(result)) return result;

    ComPtr<IFileOperation> operation;
    result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&operation));
    if (FAILED(result)) return result;
    result = operation->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION |
                                          FOFX_SHOWELEVATIONPROMPT | FOFX_EARLYFAILURE);
    if (FAILED(result)) return result;

    for (const std::wstring& internalPath : internalPaths) {
        PIDLIST_RELATIVE relative = nullptr;
        result = ParseRelative(root.Get(), internalPath, &relative);
        if (FAILED(result)) return result;
        ComPtr<IShellItem> source;
        result = SHCreateItemWithParent(nullptr, root.Get(), relative, IID_PPV_ARGS(&source));
        CoTaskMemFree(relative);
        if (FAILED(result)) return result;
        result = operation->CopyItem(source.Get(), destinationItem.Get(), nullptr, nullptr);
        if (FAILED(result)) return result;
    }
    result = operation->PerformOperations();
    if (SUCCEEDED(result)) {
        BOOL aborted = FALSE;
        if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    return result;
}
