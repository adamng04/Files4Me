#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propkey.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include "archive.h"
#include "resource.h"
#include "update.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"Files4Me.MainWindow";
constexpr wchar_t kSettingsClass[] = L"Files4Me.SettingsWindow";
constexpr wchar_t kLayoutClass[] = L"Files4Me.LayoutFlyout";
constexpr wchar_t kAppName[] = L"Files4Me";
constexpr wchar_t kHomeLocation[] = L"files4me:home";
constexpr wchar_t kThisPcLocation[] = L"files4me:thispc";
constexpr wchar_t kRecycleLocation[] = L"files4me:recyclebin";
constexpr wchar_t kDownloadsLocation[] = L"files4me:downloads";
constexpr wchar_t kArchiveLocationPrefix[] = L"files4me:zip:";
constexpr UINT WM_APP_ENUM_DONE = WM_APP + 1;
constexpr UINT WM_APP_JOB_UPDATE = WM_APP + 2;
constexpr UINT WM_APP_JOB_DONE = WM_APP + 3;
constexpr UINT WM_APP_DRIVES_DONE = WM_APP + 4;
constexpr UINT WM_APP_ARCHIVE_DONE = WM_APP + 5;
constexpr UINT WM_APP_ARCHIVE_EXTRACT_DONE = WM_APP + 6;
constexpr UINT WM_APP_NEW_ITEM_DONE = WM_APP + 7;
constexpr UINT WM_APP_UPDATE_DONE = WM_APP + 8;

enum CommandId : UINT {
    ID_BACK = 100, ID_FORWARD, ID_UP, ID_REFRESH, ID_NEW_FOLDER,
    ID_LIGHT, ID_DARK, ID_DUAL, ID_SWITCH_PANE,
    ID_OPEN, ID_RENAME, ID_COPY, ID_MOVE, ID_DELETE, ID_DELETE_PERMANENT,
    ID_NEW_TAB, ID_CLOSE_TAB, ID_FOCUS_PATH, ID_FOCUS_SEARCH, ID_SELECT_ALL, ID_CLIP_COPY, ID_CLIP_CUT, ID_CLIP_PASTE,
    ID_MENU_FILE, ID_MENU_EDIT, ID_MENU_VIEW, ID_MENU_HELP, ID_EXIT, ID_ABOUT, ID_SHORTCUTS, ID_CHECK_UPDATES,
    ID_SETTINGS, ID_THEME_TOGGLE, ID_VIEW_LAYOUT, ID_JOB_PAUSE, ID_JOB_CANCEL, ID_JOB_CLEAR,
    ID_MORE, ID_OPEN_WITH, ID_COPY_PATH, ID_PROPERTIES, ID_SHELL_CONTEXT, ID_EXTRACT_ALL = 0x9001,
    ID_CREATE_FOLDER = 0x9002, ID_CREATE_TEXT = 0x9003,
    ID_SORT_NAME = 0x9010, ID_SORT_EXTENSION, ID_SORT_SIZE, ID_SORT_MODIFIED,
    ID_FOLDER_PROPERTIES = 0x9014, ID_PIN_SIDEBAR, ID_UNPIN_SIDEBAR,
    ID_NEW_TEMPLATE_BASE = 0xA100, ID_NEW_TEMPLATE_LAST = 0xA1FF,
    ID_RECYCLE_RESTORE = 180, ID_RECYCLE_RESTORE_ALL, ID_RECYCLE_EMPTY, ID_SIDEBAR = 300,
    ID_VIEW_EXTRA_LARGE = 320, ID_VIEW_LARGE, ID_VIEW_MEDIUM, ID_VIEW_SMALL,
    ID_VIEW_LIST, ID_VIEW_DETAILS, ID_VIEW_TILES, ID_VIEW_CONTENT,
    ID_DRIVE_BASE = 400,
    ID_PANE_BASE = 1000,
    ID_PANE_STRIDE = 100,
    ID_TAB = 1, ID_DRIVES = 2, ID_PATH = 3, ID_SEARCH = 4,
    ID_HEADER_NAME = 10, ID_HEADER_EXT = 11, ID_HEADER_SIZE = 12, ID_HEADER_DATE = 13,
    ID_LIST = 20, ID_STATUS = 21,
    ID_SETTINGS_CATEGORY_BASE = 5000,
    ID_PREF_RESTORE = 5100, ID_PREF_CONFIRM_DELETE, ID_PREF_STATUS, ID_PREF_HIDDEN,
    ID_PREF_DIRS_FIRST, ID_PREF_EXTENSIONS, ID_PREF_CONTEXT_MENU, ID_PREF_DRAG_MOVE,
    ID_PREF_AUTO_REFRESH, ID_PREF_REFRESH_SPEED, ID_PREF_SIDEBAR_VISIBLE,
    ID_PREF_GALLERY, ID_PREF_RECYCLE, ID_PREF_NETWORK, ID_PREF_LINUX,
    ID_PREF_THEME_LIGHT, ID_PREF_THEME_DARK, ID_PREF_DUAL, ID_PREF_AUTO_UPDATES
};

enum class ThemeMode { Light, Dark };
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
enum class SortColumn { Name, Extension, Size, Modified };
enum class FileViewMode { ExtraLargeIcons, LargeIcons, MediumIcons, SmallIcons, List, Details, Tiles, Content };
enum class PaneMode { Filesystem, DriveOverview, RecycleBin, Archive };
enum class NewTemplateKind { Empty, TemplateFile, Data, Command };
enum class SidebarAction { Navigate, ShellOpen, Separator };
enum class JobKind : uint32_t { Copy, Move, Recycle, DeletePermanent, Rename, NewFolder };
enum class JobState : uint32_t { Queued, Running, Paused, Cancelling, Completed, Failed, Interrupted };
enum class ConflictPolicy : uint32_t { Ask, Replace, KeepBoth, Skip };

class UniqueKernelHandle {
public:
    explicit UniqueKernelHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueKernelHandle() { reset(); }
    UniqueKernelHandle(const UniqueKernelHandle&) = delete;
    UniqueKernelHandle& operator=(const UniqueKernelHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class UniqueFindHandle {
public:
    explicit UniqueFindHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueFindHandle() { if (*this) FindClose(value_); }
    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct OperationJob {
    uint64_t id = 0;
    JobKind kind = JobKind::Copy;
    std::atomic<JobState> state{JobState::Queued};
    std::vector<std::wstring> sources;
    std::wstring destination;
    std::wstring newName;
    std::vector<std::wstring> resourceKeys;
    ConflictPolicy conflict = ConflictPolicy::Ask;
    std::atomic_uint progressTotal{0};
    std::atomic_uint progressDone{0};
    std::atomic_bool pauseRequested{false};
    std::atomic_bool cancelRequested{false};
    std::mutex controlMutex;
    std::condition_variable controlChanged;
    std::mutex textMutex;
    std::wstring currentItem;
    std::atomic<HRESULT> result{S_OK};
    ULONGLONG created = 0;
};

struct OperationManager {
    std::mutex mutex;
    std::vector<std::shared_ptr<OperationJob>> jobs;
    std::set<std::wstring> activeRoots;
    size_t activeCount = 0;
    uint64_t nextId = 1;
    bool expanded = false;
};

struct Preferences {
    bool sidebarVisible = true;
    bool sidebarGallery = true;
    bool sidebarRecycleBin = true;
    bool sidebarNetwork = true;
    bool sidebarLinux = true;
    bool restorePaths = true;
    bool confirmPermanentDelete = true;
    bool showHiddenSystem = false;
    bool directoriesFirst = true;
    bool showExtensions = true;
    bool showStatus = true;
    bool autoRefresh = true;
    bool shellContextMenu = true;
    bool dragMoveSameDrive = true;
    bool automaticUpdates = true;
    UINT refreshMilliseconds = 1500;
};

struct SidebarItem {
    std::wstring label;
    std::wstring target;
    SidebarAction action = SidebarAction::Navigate;
    int icon = -1;
    bool pinned = false;
};

struct SettingsControl {
    HWND window = nullptr;
    int category = 0;
};

struct ThemedMenuItem {
    static constexpr uint32_t Magic = 0x344D454E;
    uint32_t magic = Magic;
    std::wstring text;
    UINT command = 0;
    bool separator = false;
    bool submenu = false;
};

struct Colors {
    COLORREF window;
    COLORREF surface;
    COLORREF text;
    COLORREF muted;
    COLORREF border;
    COLORREF button;
    COLORREF buttonPressed;
    COLORREF selection;
    COLORREF selectionText;
    COLORREF edit;
};

struct FileItem {
    std::wstring name;
    std::wstring fullPath;
    std::wstring extension;
    std::wstring type;
    ULONGLONG size = 0;
    FILETIME modified{};
    DWORD attributes = 0;
    int icon = 0;
    bool timelineHeader = false;
    int timelineBucket = -1;
    bool archiveItem = false;
    std::wstring archivePath;
    std::wstring archiveInternalPath;
    bool IsDirectory() const noexcept { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool IsActionable() const noexcept { return !timelineHeader; }
};

struct NewItemTemplate {
    UINT command = 0;
    std::wstring extension;
    std::wstring displayName;
    std::wstring defaultBaseName;
    NewTemplateKind kind = NewTemplateKind::Empty;
    std::wstring source;
    std::vector<BYTE> data;
    int icon = -1;
};

struct NewItemResult {
    int pane = -1;
    HRESULT result = E_FAIL;
    std::wstring path;
};

struct DriveItem {
    std::wstring root;
    std::wstring label;
    std::wstring filesystem;
    UINT type = DRIVE_UNKNOWN;
    ULONGLONG total = 0;
    ULONGLONG free = 0;
    int icon = 0;
    bool available = false;
};

struct RecycleItem {
    std::wstring name;
    std::wstring originalLocation;
    FILETIME deleted{};
    ULONGLONG size = 0;
    int icon = 0;
    ComPtr<IShellItem2> shellItem;
};

struct TabState {
    std::wstring path;
    std::vector<std::wstring> back;
    std::vector<std::wstring> forward;
};

struct Pane {
    HWND tab = nullptr;
    HWND drives = nullptr;
    HWND path = nullptr;
    HWND search = nullptr;
    std::array<HWND, 4> headers{};
    HWND list = nullptr;
    HWND status = nullptr;
    IDropTarget* dropTarget = nullptr;
    std::vector<TabState> tabs;
    std::vector<FileItem> items;
    std::vector<FileItem> sourceItems;
    std::vector<FileItem> timelineItems;
    std::wstring filterText;
    std::array<bool, 8> collapsedTimelineGroups{};
    bool timeline = false;
    std::wstring archivePath;
    std::wstring archiveInternalPath;
    int activeTab = 0;
    SortColumn sort = SortColumn::Name;
    FileViewMode view = FileViewMode::Details;
    PaneMode mode = PaneMode::Filesystem;
    std::vector<DriveItem> driveItems;
    std::vector<RecycleItem> recycleItems;
    bool ascending = true;
    bool enumerating = false;
    FILETIME directoryStamp{};
    bool dragCandidate = false;
    POINT dragStart{};
    int hoveredCheckboxItem = -1;
    std::array<int, 4> columnWidths{};
    int columnLayoutWidth = 0;
    PaneMode columnLayoutMode = PaneMode::Filesystem;
    bool headerResizing = false;
    int resizeColumn = -1;
    int resizeStartX = 0;
    int resizeStartLeft = 0;
    int resizeStartRight = 0;
    std::atomic_uint64_t generation{0};
    std::wstring pendingRenamePath;
};

struct EnumResult {
    int pane = 0;
    uint64_t generation = 0;
    std::wstring path;
    std::vector<FileItem> items;
    DWORD error = ERROR_SUCCESS;
};

struct DriveResult {
    int pane = 0;
    uint64_t generation = 0;
    std::vector<DriveItem> items;
};

struct ArchiveResult {
    int pane = 0;
    uint64_t generation = 0;
    std::wstring archivePath;
    std::wstring internalPath;
    std::vector<FileItem> items;
    HRESULT result = S_OK;
};

struct ArchiveExtractResult {
    HRESULT result = E_FAIL;
    std::wstring openPath;
    int pane = -1;
    bool openInFiles4Me = false;
};

struct IconCache {
    HIMAGELIST images = nullptr;
    int pixels = 0;
    int folderIndex = -1;
    std::vector<std::pair<int, int>> indexes;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    std::array<Pane, 2> panes;
    std::array<HWND, 8> commandButtons{};
    std::array<HWND, 7> selectionButtons{};
    std::array<HWND, 4> menuButtons{};
    std::array<HWND, 3> operationButtons{};
    HWND tooltips = nullptr;
    HWND layoutPopup = nullptr;
    std::array<HWND, 8> layoutButtons{};
    HWND layoutCaption = nullptr;
    HWND sidebar = nullptr;
    std::vector<SidebarItem> sidebarItems;
    std::vector<std::wstring> pinnedFolders;
    std::wstring pendingSidebarTarget;
    Preferences preferences;
    HWND settingsWindow = nullptr;
    int settingsCategory = 0;
    std::vector<SettingsControl> settingsControls;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    HFONT fileFont = nullptr;
    HFONT iconFont = nullptr;
    HANDLE materialFontResource = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH editBrush = nullptr;
    HIMAGELIST systemImages = nullptr;
    HIMAGELIST systemImagesLarge = nullptr;
    HIMAGELIST checkboxImages = nullptr;
    HIMAGELIST sidebarImages = nullptr;
    std::vector<std::pair<int, int>> sidebarIconIndexes;
    int genericFileIcon = -1;
    std::mutex associationIconMutex;
    std::vector<std::pair<std::wstring, int>> associationIcons;
    std::array<ComPtr<IImageList>, 3> iconSources{};
    std::array<IconCache, 3> iconCaches{};
    IconCache smallIconCache{};
    ThemeMode theme = ThemeMode::Dark;
    Colors colors{};
    int activePane = 0;
    bool dualPane = true;
    bool selectionActionsVisible = false;
    HWND hoveredToolbarButton = nullptr;
    bool closingAfterOperations = false;
    UINT dpi = 96;
    std::wstring iniPath;
    std::wstring jobsPath;
    std::wstring stateDirectory;
    std::atomic_bool updateChecking{false};
    OperationManager operations;
    std::set<std::wstring> cutPaths;
    ComPtr<IContextMenu2> activeContextMenu2;
    ComPtr<IContextMenu3> activeContextMenu3;
    std::vector<std::unique_ptr<ThemedMenuItem>> activeMenuItems;
    std::vector<NewItemTemplate> newItemTemplates;
};

AppState g_app;

void Navigate(int paneIndex, const std::wstring& requested, bool addHistory = true);
void ShowError(HRESULT result, const wchar_t* action);
void LayoutWindow();
void ApplyTheme();
void RebuildCheckboxImages();
void RebuildSidebarIconCache();
void ApplyPaneFilter(int paneIndex, bool preserveSelection = true);
int CachedSidebarIconIndex(int systemIndex);
void RefreshAll();
void SaveSettings();
void UpdateSelectionCommands();
void ApplyPaneView(int paneIndex, FileViewMode mode);
void StartArchiveEnumeration(int paneIndex, const std::wstring& archivePath, const std::wstring& internalPath);
bool HexDecode(const std::wstring& value, std::wstring& output);
std::wstring ArchiveCacheFolder();
std::wstring ArchiveExtractedPath(const std::wstring& destination, const std::wstring& internalPath);
void ShowLayoutFlyout();
void ShowNewMenu();
void CreateNewFromTemplate(UINT command);
void StartUpdateCheck(bool manual);
void ShowBackgroundContextMenu(int paneIndex, POINT point);
void PrepareThemedMenu(HMENU menu);
UINT TrackModernPopupMenu(HMENU menu, UINT flags, int x, int y, HWND owner, LPTPMPARAMS parameters = nullptr);
void DrawOwnerControl(const DRAWITEMSTRUCT* draw);
HWND CreateOwnerButton(HWND parent, UINT id, const wchar_t* text);
LRESULT CALLBACK LayoutFlyoutProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HeaderSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR paneAndColumn);

struct ShellSelection {
    ComPtr<IShellFolder> parent;
    PIDLIST_ABSOLUTE parentPidl = nullptr;
    std::vector<PIDLIST_ABSOLUTE> absolutePidls;
    std::vector<PCUITEMID_CHILD> childPidls;

    ShellSelection() = default;
    ShellSelection(const ShellSelection&) = delete;
    ShellSelection& operator=(const ShellSelection&) = delete;
    ~ShellSelection() {
        if (parentPidl) ILFree(parentPidl);
        for (PIDLIST_ABSOLUTE pidl : absolutePidls) CoTaskMemFree(pidl);
    }
};

int Scale(int value) { return MulDiv(value, static_cast<int>(g_app.dpi), 96); }

bool IsHighContrast() {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

Colors GetColors(ThemeMode mode) {
    if (IsHighContrast()) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_GRAYTEXT), GetSysColor(COLOR_WINDOWFRAME), GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_BTNSHADOW), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT),
                GetSysColor(COLOR_WINDOW)};
    }
    if (mode == ThemeMode::Dark) {
        return {RGB(28, 28, 28), RGB(36, 36, 36), RGB(248, 248, 248), RGB(184, 184, 184),
                RGB(68, 68, 68), RGB(52, 52, 52), RGB(66, 66, 66), RGB(0, 95, 184),
                RGB(255, 255, 255), RGB(38, 38, 38)};
    }
    return {RGB(246, 246, 246), RGB(255, 255, 255), RGB(24, 24, 24), RGB(96, 96, 96),
            RGB(218, 218, 218), RGB(235, 235, 235), RGB(222, 222, 222), RGB(204, 228, 247),
            RGB(15, 15, 15), RGB(255, 255, 255)};
}

COLORREF ToolbarColor() {
    if (IsHighContrast()) return GetSysColor(COLOR_BTNFACE);
    return g_app.theme == ThemeMode::Dark ? RGB(38, 38, 38) : RGB(232, 232, 232);
}

void ReleaseBrush(HBRUSH& brush) {
    if (brush) DeleteObject(brush);
    brush = nullptr;
}

void RebuildFonts() {
    if (g_app.font) DeleteObject(g_app.font);
    if (g_app.fontBold) DeleteObject(g_app.fontBold);
    if (g_app.fileFont) DeleteObject(g_app.fileFont);
    if (g_app.iconFont) DeleteObject(g_app.iconFont);
    const int height = -MulDiv(10, static_cast<int>(g_app.dpi), 72);
    g_app.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    g_app.fontBold = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    const int fileHeight = -MulDiv(11, static_cast<int>(g_app.dpi), 72);
    g_app.fileFont = CreateFontW(fileHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    g_app.iconFont = CreateFontW(-Scale(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Material Icons");
}

void LoadMaterialIconFont() {
    HRSRC resource = FindResourceW(g_app.instance, MAKEINTRESOURCEW(IDR_MATERIAL_ICONS), RT_RCDATA);
    if (!resource) return;
    HGLOBAL loaded = LoadResource(g_app.instance, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(g_app.instance, resource);
    DWORD fonts = 0;
    if (data && size) g_app.materialFontResource = AddFontMemResourceEx(const_cast<void*>(data), size, nullptr, &fonts);
}

void ApplyFont(HWND window, bool bold = false) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_app.fontBold : g_app.font), TRUE);
}

std::wstring ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return L".";
    PathRemoveFileSpecW(buffer.data());
    return buffer.data();
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID folderId, const wchar_t* fallback = L"") {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &value))) {
        std::wstring path(value);
        CoTaskMemFree(value);
        return path;
    }
    return fallback;
}

std::wstring DefaultPath() {
    return KnownFolderPath(FOLDERID_Profile, L"C:\\");
}

std::wstring NormalizePath(const std::wstring& input) {
    if (input.empty()) return {};
    std::wstring expanded(32768, L'\0');
    DWORD expandedLength = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (expandedLength == 0 || expandedLength > expanded.size()) expanded = input;
    else expanded.resize(expandedLength - 1);

    std::wstring full(32768, L'\0');
    DWORD length = GetFullPathNameW(expanded.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length == 0 || length >= full.size()) return {};
    full.resize(length);
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return full;
}

std::wstring ExtendedPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& name) {
    if (base.empty()) return name;
    return base + (base.back() == L'\\' ? L"" : L"\\") + name;
}

std::wstring ParentPath(const std::wstring& path) {
    std::vector<wchar_t> value(path.begin(), path.end());
    value.push_back(L'\0');
    if (!PathRemoveFileSpecW(value.data())) return path;
    std::wstring result(value.data());
    return result.empty() ? path : result;
}

std::wstring DisplayNameForPath(const std::wstring& path) {
    if (_wcsicmp(path.c_str(), kHomeLocation) == 0) return L"Home";
    if (_wcsicmp(path.c_str(), kThisPcLocation) == 0) return L"This PC";
    if (_wcsicmp(path.c_str(), kRecycleLocation) == 0) return L"Recycle Bin";
    if (path.rfind(kArchiveLocationPrefix, 0) == 0) {
        const std::wstring payload = path.substr(ARRAYSIZE(kArchiveLocationPrefix) - 1);
        const size_t separator = payload.find(L'!');
        std::wstring archive, internal;
        if (separator != std::wstring::npos && HexDecode(payload.substr(0, separator), archive) &&
            HexDecode(payload.substr(separator + 1), internal)) {
            std::wstring label = PathFindFileNameW(archive.c_str());
            if (!internal.empty()) label += L"\\" + internal;
            return label;
        }
    }
    if (PathIsRootW(path.c_str())) return path;
    const wchar_t* name = PathFindFileNameW(path.c_str());
    return (name && *name) ? name : path;
}

std::wstring HexEncode(const std::wstring& value) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring output;
    output.reserve(value.size() * 4);
    for (wchar_t character : value) {
        const unsigned number = static_cast<unsigned>(character);
        output.push_back(digits[(number >> 12) & 15]); output.push_back(digits[(number >> 8) & 15]);
        output.push_back(digits[(number >> 4) & 15]); output.push_back(digits[number & 15]);
    }
    return output;
}

bool HexDecode(const std::wstring& value, std::wstring& output) {
    if (value.size() % 4 != 0) return false;
    auto digit = [](wchar_t character) -> int {
        if (character >= L'0' && character <= L'9') return character - L'0';
        if (character >= L'A' && character <= L'F') return character - L'A' + 10;
        if (character >= L'a' && character <= L'f') return character - L'a' + 10;
        return -1;
    };
    output.clear(); output.reserve(value.size() / 4);
    for (size_t index = 0; index < value.size(); index += 4) {
        int value0 = digit(value[index]), value1 = digit(value[index + 1]);
        int value2 = digit(value[index + 2]), value3 = digit(value[index + 3]);
        if (value0 < 0 || value1 < 0 || value2 < 0 || value3 < 0) return false;
        output.push_back(static_cast<wchar_t>((value0 << 12) | (value1 << 8) | (value2 << 4) | value3));
    }
    return true;
}

std::wstring ArchiveLocation(const std::wstring& archivePath, const std::wstring& internalPath = {}) {
    return std::wstring(kArchiveLocationPrefix) + HexEncode(archivePath) + L"!" + HexEncode(internalPath);
}

bool ParseArchiveLocation(const std::wstring& value, std::wstring& archivePath, std::wstring& internalPath) {
    if (value.rfind(kArchiveLocationPrefix, 0) != 0) return false;
    const std::wstring payload = value.substr(ARRAYSIZE(kArchiveLocationPrefix) - 1);
    const size_t separator = payload.find(L'!');
    return separator != std::wstring::npos && HexDecode(payload.substr(0, separator), archivePath) &&
           HexDecode(payload.substr(separator + 1), internalPath) && !archivePath.empty();
}

bool IsArchiveLocation(const std::wstring& value) {
    return value.rfind(kArchiveLocationPrefix, 0) == 0;
}

bool IsDownloadsPath(const std::wstring& path) {
    const std::wstring downloads = NormalizePath(KnownFolderPath(FOLDERID_Downloads));
    return !downloads.empty() && _wcsicmp(downloads.c_str(), path.c_str()) == 0;
}

bool IsVirtualLocation(const std::wstring& path) {
    return _wcsicmp(path.c_str(), kHomeLocation) == 0 || _wcsicmp(path.c_str(), kThisPcLocation) == 0 ||
           _wcsicmp(path.c_str(), kRecycleLocation) == 0;
}

std::wstring FormatSize(ULONGLONG bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    do { value /= 1024.0; ++unit; } while (value >= 1024.0 && unit < 3);
    wchar_t text[64]{};
    StringCchPrintfW(text, ARRAYSIZE(text), value >= 100.0 ? L"%.0f %s" : L"%.1f %s", value, units[unit]);
    return text;
}

std::wstring FormatTime(const FILETIME& time) {
    FILETIME local{};
    SYSTEMTIME system{};
    if (!FileTimeToLocalFileTime(&time, &local) || !FileTimeToSystemTime(&local, &system)) return {};
    wchar_t date[32]{}, clock[32]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &system, nullptr, date, ARRAYSIZE(date), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &system, nullptr, clock, ARRAYSIZE(clock));
    return std::wstring(date) + L" " + clock;
}

std::wstring ReadAssociationRegistryString(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    DWORD type = 0, bytes = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (RegGetValueW(root, subkey.c_str(), value, flags, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(root, subkey.c_str(), value, flags, &type, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    std::wstring result = buffer.data();
    if (type == REG_EXPAND_SZ && !result.empty()) {
        DWORD required = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
        if (required) {
            std::vector<wchar_t> expanded(required);
            if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(), required)) result = expanded.data();
        }
    }
    return result;
}

std::wstring RegisteredApplicationPath(const std::wstring& extension) {
    std::wstring progId = ReadAssociationRegistryString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + extension + L"\\UserChoice",
        L"ProgId");
    if (progId.empty()) progId = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, extension, nullptr);
    if (progId.empty()) return {};

    const std::wstring originalProgId = progId;
    const std::wstring current = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, progId + L"\\CurVer", nullptr);
    if (!current.empty()) progId = current;
    std::wstring command = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, progId + L"\\shell\\open\\command", nullptr);
    if (command.empty() && progId != originalProgId)
        command = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, originalProgId + L"\\shell\\open\\command", nullptr);
    if (!command.empty()) {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
        if (arguments && count > 0) {
            std::wstring executable = arguments[0];
            LocalFree(arguments);
            if (GetFileAttributesW(executable.c_str()) != INVALID_FILE_ATTRIBUTES) return executable;
        } else if (arguments) LocalFree(arguments);
    }

    std::wstring iconLocation = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, progId + L"\\DefaultIcon", nullptr);
    if (iconLocation.empty() && progId != originalProgId)
        iconLocation = ReadAssociationRegistryString(HKEY_CLASSES_ROOT, originalProgId + L"\\DefaultIcon", nullptr);
    if (!iconLocation.empty()) {
        std::vector<wchar_t> mutableLocation(iconLocation.begin(), iconLocation.end());
        mutableLocation.push_back(L'\0');
        PathParseIconLocationW(mutableLocation.data());
        std::wstring iconFile = mutableLocation.data();
        if (GetFileAttributesW(iconFile.c_str()) != INVALID_FILE_ATTRIBUTES) return iconFile;
    }
    return {};
}

int AssociatedApplicationIcon(const std::wstring& path) {
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension || !*extension || _wcsicmp(extension, L".exe") == 0 ||
        _wcsicmp(extension, L".ico") == 0 || _wcsicmp(extension, L".lnk") == 0) return -1;
    std::wstring key = extension;
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    {
        std::lock_guard lock(g_app.associationIconMutex);
        for (const auto& [cachedExtension, icon] : g_app.associationIcons)
            if (cachedExtension == key) return icon;
    }

    wchar_t executable[32768]{};
    DWORD length = ARRAYSIZE(executable);
    const ASSOCF flags = static_cast<ASSOCF>(ASSOCF_INIT_DEFAULTTOSTAR | ASSOCF_NOTRUNCATE);
    int icon = -1;
    if (SUCCEEDED(AssocQueryStringW(flags, ASSOCSTR_EXECUTABLE, extension, nullptr, executable, &length)) &&
        executable[0] != L'\0') {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(executable, 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            icon = info.iIcon;
    }
    if (icon < 0) {
        const std::wstring registeredApplication = RegisteredApplicationPath(key);
        if (!registeredApplication.empty()) {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(registeredApplication.c_str(), 0, &info, sizeof(info),
                               SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) icon = info.iIcon;
        }
    }
    {
        std::lock_guard lock(g_app.associationIconMutex);
        for (const auto& [cachedExtension, cachedIcon] : g_app.associationIcons)
            if (cachedExtension == key) return cachedIcon;
        g_app.associationIcons.emplace_back(std::move(key), icon);
    }
    return icon;
}

int IconFor(const std::wstring& path, DWORD attributes) {
    SHFILEINFOW info{};
    // Query the real Shell item first. SHGFI_USEFILEATTRIBUTES deliberately skips
    // file access, so using it unconditionally loses embedded executable icons and
    // icons supplied by registered applications/Shell handlers.
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && info.iIcon == g_app.genericFileIcon) {
            const int associatedIcon = AssociatedApplicationIcon(path);
            if (associatedIcon >= 0 && associatedIcon != g_app.genericFileIcon) return associatedIcon;
        }
        return info.iIcon;
    }

    // Keep enumeration resilient for inaccessible, offline, or disappearing items.
    // In that case Windows can still resolve the registered type icon from the name.
    info = {};
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && info.iIcon == g_app.genericFileIcon) {
            const int associatedIcon = AssociatedApplicationIcon(path);
            if (associatedIcon >= 0 && associatedIcon != g_app.genericFileIcon) return associatedIcon;
        }
        return info.iIcon;
    }
    return 0;
}

int IconForShellName(const wchar_t* parsingName) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(parsingName, nullptr, &pidl, 0, nullptr)) || !pidl) return 0;
    SHFILEINFOW info{};
    const DWORD_PTR result = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info),
                                            SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    CoTaskMemFree(pidl);
    return result ? info.iIcon : 0;
}

void AddSidebarItem(const std::wstring& label, const std::wstring& target, SidebarAction action, int icon, bool pinned = false) {
    g_app.sidebarItems.push_back({label, target, action, icon, pinned});
}

void AddSidebarSeparator() { AddSidebarItem(L"", L"", SidebarAction::Separator, -1); }

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool IsPinnedFolder(const std::wstring& path) {
    return std::any_of(g_app.pinnedFolders.begin(), g_app.pinnedFolders.end(), [&](const std::wstring& pinned) {
        return SamePath(pinned, path);
    });
}

void RebuildSidebar() {
    if (!g_app.sidebar) return;
    ListView_DeleteAllItems(g_app.sidebar);
    g_app.sidebarItems.clear();
    const std::wstring profile = DefaultPath();
    const std::wstring pictures = KnownFolderPath(FOLDERID_Pictures, profile.c_str());
    AddSidebarItem(L"Home", kHomeLocation, SidebarAction::Navigate, IconFor(profile, FILE_ATTRIBUTE_DIRECTORY));
    if (g_app.preferences.sidebarGallery) AddSidebarItem(L"Gallery", pictures, SidebarAction::Navigate, IconFor(pictures, FILE_ATTRIBUTE_DIRECTORY));
    AddSidebarSeparator();
    const struct { const wchar_t* label; const KNOWNFOLDERID* id; } folders[] = {
        {L"Desktop", &FOLDERID_Desktop},
        {L"Documents", &FOLDERID_Documents}, {L"Pictures", &FOLDERID_Pictures},
        {L"Music", &FOLDERID_Music}, {L"Videos", &FOLDERID_Videos}
    };
    for (size_t index = 0; index < ARRAYSIZE(folders); ++index) {
        const auto& folder = folders[index];
        const std::wstring path = KnownFolderPath(*folder.id);
        if (!path.empty()) AddSidebarItem(folder.label, path, SidebarAction::Navigate, IconFor(path, FILE_ATTRIBUTE_DIRECTORY));
        if (index == 0) {
            const std::wstring downloads = KnownFolderPath(FOLDERID_Downloads);
            if (!downloads.empty()) AddSidebarItem(L"Downloads", kDownloadsLocation, SidebarAction::Navigate,
                                                   IconFor(downloads, FILE_ATTRIBUTE_DIRECTORY));
        }
    }
    if (g_app.preferences.sidebarRecycleBin) {
        AddSidebarItem(L"Recycle Bin", kRecycleLocation, SidebarAction::Navigate,
                       IconForShellName(L"::{645FF040-5081-101B-9F08-00AA002F954E}"));
    }
    for (const std::wstring& pinned : g_app.pinnedFolders) {
        const DWORD attributes = GetFileAttributesW(ExtendedPath(pinned).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        std::wstring label = PathFindFileNameW(pinned.c_str());
        if (label.empty()) label = pinned;
        AddSidebarItem(label, pinned, SidebarAction::Navigate, IconFor(pinned, FILE_ATTRIBUTE_DIRECTORY), true);
    }
    AddSidebarSeparator();
    AddSidebarItem(L"This PC", kThisPcLocation, SidebarAction::Navigate,
                   IconForShellName(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"));
    wchar_t drives[512]{};
    if (GetLogicalDriveStringsW(ARRAYSIZE(drives), drives) != 0) {
        for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) {
            wchar_t volume[64]{};
            GetVolumeInformationW(drive, volume, ARRAYSIZE(volume), nullptr, nullptr, nullptr, nullptr, 0);
            std::wstring label = volume[0] ? std::wstring(volume) + L" (" + std::wstring(drive, 2) + L")" : L"Local Disk (" + std::wstring(drive, 2) + L")";
            AddSidebarItem(label, drive, SidebarAction::Navigate, IconFor(drive, FILE_ATTRIBUTE_DIRECTORY));
        }
    }
    if (g_app.preferences.sidebarNetwork) {
        AddSidebarItem(L"Network", L"shell:NetworkPlacesFolder", SidebarAction::ShellOpen,
                       IconForShellName(L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"));
    }
    if (g_app.preferences.sidebarLinux) {
        AddSidebarItem(L"Linux", L"shell:Linux", SidebarAction::ShellOpen, IconFor(L"\\\\wsl$", FILE_ATTRIBUTE_DIRECTORY));
    }
    for (int index = 0; index < static_cast<int>(g_app.sidebarItems.size()); ++index) {
        const SidebarItem& sidebarItem = g_app.sidebarItems[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(sidebarItem.label.c_str());
        item.iImage = sidebarItem.icon >= 0 ? CachedSidebarIconIndex(sidebarItem.icon) : -1;
        item.lParam = index;
        ListView_InsertItem(g_app.sidebar, &item);
    }
}

void ActivateSidebarItem(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.sidebarItems.size())) return;
    const SidebarItem& item = g_app.sidebarItems[index];
    if (item.action == SidebarAction::Navigate) Navigate(g_app.activePane, item.target);
    else if (item.action == SidebarAction::ShellOpen) {
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.hwnd = g_app.window;
        execute.lpVerb = L"open";
        execute.lpFile = item.target.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execute)) ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Open shortcut");
    }
}

bool CompareItems(const FileItem& left, const FileItem& right, SortColumn column, bool ascending, bool directoriesFirst = true) {
    if (directoriesFirst && left.IsDirectory() != right.IsDirectory()) return left.IsDirectory();
    int result = 0;
    switch (column) {
    case SortColumn::Extension: result = StrCmpLogicalW(left.extension.c_str(), right.extension.c_str()); break;
    case SortColumn::Size:
        result = left.size < right.size ? -1 : (left.size > right.size ? 1 : 0); break;
    case SortColumn::Modified: result = CompareFileTime(&left.modified, &right.modified); break;
    default: result = StrCmpLogicalW(left.name.c_str(), right.name.c_str()); break;
    }
    if (result == 0) result = StrCmpLogicalW(left.name.c_str(), right.name.c_str());
    return ascending ? result < 0 : result > 0;
}

ULONGLONG CalendarDayValue(const SYSTEMTIME& source) {
    SYSTEMTIME date{};
    date.wYear = source.wYear; date.wMonth = source.wMonth; date.wDay = source.wDay;
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&date, &fileTime)) return 0;
    ULARGE_INTEGER value{}; value.LowPart = fileTime.dwLowDateTime; value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart / 864000000000ULL;
}

int TimelineBucketFor(const FILETIME& modified) {
    FILETIME localFileTime{};
    SYSTEMTIME item{}, now{};
    FileTimeToLocalFileTime(&modified, &localFileTime);
    if (!FileTimeToSystemTime(&localFileTime, &item)) return 7;
    GetLocalTime(&now);
    const ULONGLONG today = CalendarDayValue(now), itemDay = CalendarDayValue(item);
    const ULONGLONG rawDifference = today >= itemDay ? today - itemDay : 0;
    const int difference = static_cast<int>((std::min<ULONGLONG>)(rawDifference, 32767));
    if (difference == 0) return 0;
    if (difference == 1) return 1;

    DWORD firstDay = 0;
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                         reinterpret_cast<LPWSTR>(&firstDay), sizeof(firstDay) / sizeof(wchar_t))) firstDay = 0;
    const int firstWeekday = static_cast<int>((firstDay + 1) % 7); // locale: Monday=0; SYSTEMTIME: Sunday=0
    const int daysIntoWeek = (static_cast<int>(now.wDayOfWeek) - firstWeekday + 7) % 7;
    if (difference <= daysIntoWeek) return 2;
    if (difference <= daysIntoWeek + 7) return 3;
    if (item.wYear == now.wYear && item.wMonth == now.wMonth) return 4;
    int previousMonth = static_cast<int>(now.wMonth) - 1;
    int previousYear = now.wYear;
    if (previousMonth == 0) { previousMonth = 12; --previousYear; }
    if (item.wYear == previousYear && item.wMonth == previousMonth) return 5;
    if (item.wYear == now.wYear) return 6;
    return 7;
}

const wchar_t* TimelineBucketName(int bucket) {
    static constexpr const wchar_t* names[] = {
        L"Today", L"Yesterday", L"Earlier this week", L"Last week",
        L"Earlier this month", L"Last month", L"Earlier this year", L"A long time ago"
    };
    return bucket >= 0 && bucket < ARRAYSIZE(names) ? names[bucket] : L"Older";
}

void RebuildTimelineRows(Pane& pane, const std::vector<std::wstring>& selectedPaths = {}) {
    std::array<std::vector<FileItem>, 8> groups;
    for (FileItem item : pane.timelineItems) {
        if (!pane.filterText.empty() && !StrStrIW(item.name.c_str(), pane.filterText.c_str()) &&
            !StrStrIW(item.extension.c_str(), pane.filterText.c_str()) && !StrStrIW(item.type.c_str(), pane.filterText.c_str())) continue;
        groups[TimelineBucketFor(item.modified)].push_back(std::move(item));
    }
    const bool reverseGroups = pane.sort == SortColumn::Modified && pane.ascending;
    pane.items.clear();
    for (int step = 0; step < 8; ++step) {
        const int bucket = reverseGroups ? 7 - step : step;
        auto& group = groups[bucket];
        if (group.empty()) continue;
        std::sort(group.begin(), group.end(), [&](const FileItem& left, const FileItem& right) {
            return CompareItems(left, right, pane.sort, pane.ascending, false);
        });
        FileItem header;
        header.name = TimelineBucketName(bucket);
        header.timelineHeader = true;
        header.timelineBucket = bucket;
        header.size = group.size();
        pane.items.push_back(std::move(header));
        if (!pane.collapsedTimelineGroups[bucket]) {
            for (FileItem& item : group) pane.items.push_back(std::move(item));
        }
    }
    ListView_SetItemCountEx(pane.list, static_cast<int>(pane.items.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    for (int row = 0; row < static_cast<int>(pane.items.size()); ++row) {
        if (pane.items[row].timelineHeader) continue;
        if (std::find_if(selectedPaths.begin(), selectedPaths.end(), [&](const std::wstring& path) {
                return _wcsicmp(path.c_str(), pane.items[row].fullPath.c_str()) == 0;
            }) != selectedPaths.end()) {
            ListView_SetItemState(pane.list, row, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    InvalidateRect(pane.list, nullptr, FALSE);
}

void StartEnumeration(int paneIndex, const std::wstring& path) {
    Pane& pane = g_app.panes[paneIndex];
    pane.hoveredCheckboxItem = -1;
    const uint64_t generation = ++pane.generation;
    const SortColumn sortColumn = pane.sort;
    const bool sortAscending = pane.ascending;
    const bool showHiddenSystem = g_app.preferences.showHiddenSystem;
    const bool directoriesFirst = g_app.preferences.directoriesFirst;
    pane.enumerating = true;
    SetWindowTextW(pane.status, L"Loading…");
    ListView_SetItemCountEx(pane.list, 0, LVSICF_NOINVALIDATEALL);

    std::thread([paneIndex, generation, path, sortColumn, sortAscending, showHiddenSystem, directoriesFirst, target = g_app.window] {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = std::make_unique<EnumResult>();
        result->pane = paneIndex;
        result->generation = generation;
        result->path = path;

        const std::wstring pattern = ExtendedPath(JoinPath(path, L"*"));
        WIN32_FIND_DATAW data{};
        UniqueFindHandle find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                              nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find) {
            result->error = GetLastError();
        } else {
            do {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                if (!showHiddenSystem && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) continue;
                FileItem item;
                item.name = data.cFileName;
                item.fullPath = JoinPath(path, item.name);
                item.attributes = data.dwFileAttributes;
                item.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                item.modified = data.ftLastWriteTime;
                if (!item.IsDirectory()) {
                    const wchar_t* extension = PathFindExtensionW(item.name.c_str());
                    if (extension && *extension && extension != item.name.c_str()) item.extension = extension + 1;
                }
                item.type = item.IsDirectory() ? L"Folder" : (item.extension.empty() ? L"File" : item.extension + L" file");
                item.icon = IconFor(item.fullPath, item.attributes);
                result->items.push_back(std::move(item));
            } while (FindNextFileW(find.get(), &data));
            const DWORD lastError = GetLastError();
            if (lastError != ERROR_NO_MORE_FILES) result->error = lastError;
        }

        std::sort(result->items.begin(), result->items.end(), [sortColumn, sortAscending, directoriesFirst](const FileItem& a, const FileItem& b) {
            return CompareItems(a, b, sortColumn, sortAscending, directoriesFirst);
        });
        EnumResult* raw = result.release();
        if (!PostMessageW(target, WM_APP_ENUM_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        if (SUCCEEDED(comResult)) CoUninitialize();
    }).detach();
}

void StartArchiveEnumeration(int paneIndex, const std::wstring& archivePath, const std::wstring& internalPath) {
    Pane& pane = g_app.panes[paneIndex];
    pane.hoveredCheckboxItem = -1;
    const uint64_t generation = ++pane.generation;
    pane.enumerating = true;
    SetWindowTextW(pane.status, L"Reading archive...");
    ListView_SetItemCountEx(pane.list, 0, LVSICF_NOINVALIDATEALL);
    std::thread([paneIndex, generation, archivePath, internalPath, target = g_app.window] {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = std::make_unique<ArchiveResult>();
        result->pane = paneIndex; result->generation = generation;
        result->archivePath = archivePath; result->internalPath = internalPath;
        std::vector<ArchiveEntryData> entries;
        result->result = SUCCEEDED(comResult) ? EnumerateZipFolder(archivePath, internalPath, entries) : comResult;
        for (ArchiveEntryData& entry : entries) {
            FileItem item;
            item.name = std::move(entry.name); item.extension = std::move(entry.extension);
            item.type = std::move(entry.type); item.size = entry.size; item.modified = entry.modified;
            item.attributes = entry.attributes; item.icon = entry.icon; item.archiveItem = true;
            item.archivePath = archivePath; item.archiveInternalPath = std::move(entry.internalPath);
            result->items.push_back(std::move(item));
        }
        std::sort(result->items.begin(), result->items.end(), [](const FileItem& a, const FileItem& b) {
            return CompareItems(a, b, SortColumn::Name, true, true);
        });
        ArchiveResult* raw = result.release();
        if (!PostMessageW(target, WM_APP_ARCHIVE_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        if (SUCCEEDED(comResult)) CoUninitialize();
    }).detach();
}

const PROPERTYKEY kRecycleDeletedFromKey = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2,0x7E,0x00,0xC0,0x4F,0xC3,0x08,0x71}}, 2};
const PROPERTYKEY kRecycleDateDeletedKey = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2,0x7E,0x00,0xC0,0x4F,0xC3,0x08,0x71}}, 3};

std::wstring DriveTypeText(UINT type) {
    switch (type) {
    case DRIVE_REMOVABLE: return L"Removable drive";
    case DRIVE_REMOTE: return L"Network drive";
    case DRIVE_CDROM: return L"Optical drive";
    case DRIVE_RAMDISK: return L"RAM drive";
    default: return L"Local disk";
    }
}

void StartDriveOverview(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    pane.hoveredCheckboxItem = -1;
    pane.mode = PaneMode::DriveOverview;
    pane.timeline = false; pane.timelineItems.clear(); pane.archivePath.clear(); pane.archiveInternalPath.clear();
    pane.items.clear(); pane.sourceItems.clear(); pane.recycleItems.clear(); pane.driveItems.clear();
    const uint64_t generation = ++pane.generation;
    pane.enumerating = true;
    SetWindowTheme(pane.list, L"", L"");
    ListView_SetView(pane.list, LV_VIEW_TILE);
    ListView_SetItemCountEx(pane.list, 0, 0);
    SetWindowTextW(pane.status, L"Loading drives...");
    HWND target = g_app.window;
    std::thread([paneIndex, generation, target] {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = std::make_unique<DriveResult>(); result->pane = paneIndex; result->generation = generation;
        wchar_t drives[512]{};
        if (GetLogicalDriveStringsW(ARRAYSIZE(drives), drives)) {
            for (const wchar_t* root = drives; *root; root += wcslen(root) + 1) {
                DriveItem item; item.root = root; item.type = GetDriveTypeW(root);
                wchar_t volume[128]{}, filesystem[64]{};
                if (GetVolumeInformationW(root, volume, ARRAYSIZE(volume), nullptr, nullptr, nullptr,
                                          filesystem, ARRAYSIZE(filesystem))) {
                    item.label = volume; item.filesystem = filesystem;
                }
                if (item.label.empty()) item.label = DriveTypeText(item.type);
                ULARGE_INTEGER available{}, total{}, free{};
                if (GetDiskFreeSpaceExW(root, &available, &total, &free)) {
                    item.available = true; item.total = total.QuadPart; item.free = available.QuadPart;
                }
                item.icon = IconFor(item.root, FILE_ATTRIBUTE_DIRECTORY);
                result->items.push_back(std::move(item));
            }
        }
        DriveResult* raw = result.release();
        if (!PostMessageW(target, WM_APP_DRIVES_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        if (SUCCEEDED(comResult)) CoUninitialize();
    }).detach();
}

void LoadRecycleBin(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    pane.hoveredCheckboxItem = -1;
    ++pane.generation; pane.enumerating = false; pane.mode = PaneMode::RecycleBin;
    pane.timeline = false; pane.timelineItems.clear(); pane.archivePath.clear(); pane.archiveInternalPath.clear();
    pane.items.clear(); pane.sourceItems.clear(); pane.driveItems.clear(); pane.recycleItems.clear();
    const bool dark = g_app.theme == ThemeMode::Dark && !IsHighContrast();
    SetWindowTheme(pane.list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    ComPtr<IShellFolder> desktop, folder;
    PIDLIST_ABSOLUTE binPidl = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) &&
        SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &binPidl)) && binPidl &&
        SUCCEEDED(desktop->BindToObject(binPidl, nullptr, IID_PPV_ARGS(&folder)))) {
        ComPtr<IEnumIDList> enumerator;
        if (SUCCEEDED(folder->EnumObjects(g_app.window, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumerator))) {
            PITEMID_CHILD child = nullptr; ULONG fetched = 0;
            while (enumerator->Next(1, &child, &fetched) == S_OK) {
                PIDLIST_ABSOLUTE absolute = ILCombine(binPidl, child);
                RecycleItem recycled;
                if (absolute && SUCCEEDED(SHCreateItemFromIDList(absolute, IID_PPV_ARGS(&recycled.shellItem)))) {
                    PWSTR value = nullptr;
                    if (SUCCEEDED(recycled.shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &value)) && value) {
                        recycled.name = value; CoTaskMemFree(value);
                    }
                    value = nullptr;
                    if (SUCCEEDED(recycled.shellItem->GetString(kRecycleDeletedFromKey, &value)) && value) {
                        recycled.originalLocation = value; CoTaskMemFree(value);
                    }
                    recycled.shellItem->GetFileTime(kRecycleDateDeletedKey, &recycled.deleted);
                    recycled.shellItem->GetUInt64(PKEY_Size, &recycled.size);
                    SHFILEINFOW info{};
                    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0, &info, sizeof(info),
                                       SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) recycled.icon = info.iIcon;
                    pane.recycleItems.push_back(std::move(recycled));
                }
                if (absolute) ILFree(absolute); CoTaskMemFree(child); child = nullptr;
            }
        }
    }
    if (binPidl) ILFree(binPidl);
    ListView_SetView(pane.list, LV_VIEW_DETAILS);
    ListView_SetImageList(pane.list, g_app.smallIconCache.images, LVSIL_SMALL);
    ListView_SetItemCountEx(pane.list, static_cast<int>(pane.recycleItems.size()), 0);
    SetWindowTextW(pane.status, (std::to_wstring(pane.recycleItems.size()) + L" items").c_str());
    InvalidateRect(pane.list, nullptr, TRUE); LayoutWindow();
}

void UpdateTabText(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    if (pane.activeTab < 0 || pane.activeTab >= static_cast<int>(pane.tabs.size())) return;
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    std::wstring text = DisplayNameForPath(pane.tabs[pane.activeTab].path);
    item.pszText = text.data();
    TabCtrl_SetItem(pane.tab, pane.activeTab, &item);
}

void Navigate(int paneIndex, const std::wstring& requested, bool addHistory) {
    Pane& pane = g_app.panes[paneIndex];
    const bool forcedDownloadsTimeline = _wcsicmp(requested.c_str(), kDownloadsLocation) == 0;
    const std::wstring resolvedRequest = forcedDownloadsTimeline ? KnownFolderPath(FOLDERID_Downloads) : requested;
    std::wstring archivePath, archiveInternalPath;
    const bool archiveLocation = ParseArchiveLocation(resolvedRequest, archivePath, archiveInternalPath);
    std::wstring path = (IsVirtualLocation(resolvedRequest) || archiveLocation) ? resolvedRequest : NormalizePath(resolvedRequest);
    if (archiveLocation && (GetFileAttributesW(ExtendedPath(archivePath).c_str()) == INVALID_FILE_ATTRIBUTES ||
                            PathMatchSpecW(archivePath.c_str(), L"*.zip") == FALSE)) {
        MessageBoxW(g_app.window, L"ZIP archive cannot be opened.", kAppName, MB_OK | MB_ICONERROR); return;
    }
    if (!archiveLocation && !IsVirtualLocation(path) && (path.empty() || GetFileAttributesW(ExtendedPath(path).c_str()) == INVALID_FILE_ATTRIBUTES ||
        (GetFileAttributesW(ExtendedPath(path).c_str()) & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
        MessageBoxW(g_app.window, L"Folder cannot be opened. Check that the path exists and access is allowed.", kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    TabState& tab = pane.tabs[pane.activeTab];
    if (addHistory && !tab.path.empty() && _wcsicmp(tab.path.c_str(), path.c_str()) != 0) {
        tab.back.push_back(tab.path);
        tab.forward.clear();
    }
    tab.path = path;
    pane.filterText.clear();
    if (pane.search) SetWindowTextW(pane.search, L"");
    SetWindowTextW(pane.path, DisplayNameForPath(path).c_str());
    UpdateTabText(paneIndex);
    if (archiveLocation) {
        pane.mode = PaneMode::Archive; pane.view = FileViewMode::Details; pane.timeline = false; pane.timelineItems.clear(); pane.sourceItems.clear();
        pane.archivePath = archivePath; pane.archiveInternalPath = archiveInternalPath;
        SendMessageW(pane.path, EM_SETREADONLY, TRUE, 0);
        SetWindowTextW(pane.path, DisplayNameForPath(path).c_str());
        SetWindowTextW(pane.headers[0], L"Name"); SetWindowTextW(pane.headers[1], L"Ext");
        SetWindowTextW(pane.headers[2], L"Size"); SetWindowTextW(pane.headers[3], L"Modified");
        ListView_SetView(pane.list, LV_VIEW_DETAILS);
        ListView_SetImageList(pane.list, g_app.smallIconCache.images, LVSIL_SMALL);
        if (paneIndex == g_app.activePane) {
            EnableWindow(g_app.commandButtons[2], TRUE); EnableWindow(g_app.commandButtons[4], TRUE);
            EnableWindow(g_app.commandButtons[6], FALSE); SetWindowTextW(g_app.commandButtons[4], L"Extract all");
        }
        StartArchiveEnumeration(paneIndex, archivePath, archiveInternalPath);
        LayoutWindow();
    } else if (_wcsicmp(path.c_str(), kHomeLocation) == 0 || _wcsicmp(path.c_str(), kThisPcLocation) == 0) {
        SendMessageW(pane.path, EM_SETREADONLY, TRUE, 0);
        SetWindowTextW(pane.headers[0], L"Devices and drives");
        if (paneIndex == g_app.activePane) {
            EnableWindow(g_app.commandButtons[2], FALSE); EnableWindow(g_app.commandButtons[4], FALSE);
            EnableWindow(g_app.commandButtons[6], FALSE); SetWindowTextW(g_app.commandButtons[4], L"New");
        }
        StartDriveOverview(paneIndex);
    } else if (_wcsicmp(path.c_str(), kRecycleLocation) == 0) {
        SendMessageW(pane.path, EM_SETREADONLY, TRUE, 0);
        if (paneIndex == g_app.activePane) {
            EnableWindow(g_app.commandButtons[2], FALSE); EnableWindow(g_app.commandButtons[4], TRUE);
            EnableWindow(g_app.commandButtons[6], FALSE); SetWindowTextW(g_app.commandButtons[4], L"Empty Bin");
        }
        SetWindowTextW(pane.headers[0], L"Name"); SetWindowTextW(pane.headers[1], L"Original location");
        SetWindowTextW(pane.headers[2], L"Date deleted"); SetWindowTextW(pane.headers[3], L"Size");
        LoadRecycleBin(paneIndex);
    } else {
        pane.mode = PaneMode::Filesystem;
        const bool enteringTimeline = forcedDownloadsTimeline || IsDownloadsPath(path);
        if (enteringTimeline && !pane.timeline) { pane.sort = SortColumn::Modified; pane.ascending = false; pane.view = FileViewMode::Details; }
        pane.timeline = enteringTimeline;
        pane.timelineItems.clear(); pane.sourceItems.clear(); pane.archivePath.clear(); pane.archiveInternalPath.clear();
        const bool dark = g_app.theme == ThemeMode::Dark && !IsHighContrast();
        SetWindowTheme(pane.list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (paneIndex == g_app.activePane) {
            EnableWindow(g_app.commandButtons[2], TRUE); EnableWindow(g_app.commandButtons[4], TRUE);
            EnableWindow(g_app.commandButtons[6], TRUE); SetWindowTextW(g_app.commandButtons[4], L"New");
        }
        SendMessageW(pane.path, EM_SETREADONLY, FALSE, 0);
        SetWindowTextW(pane.path, path.c_str());
        SetWindowTextW(pane.headers[0], L"Name"); SetWindowTextW(pane.headers[1], L"Ext");
        SetWindowTextW(pane.headers[2], L"Size"); SetWindowTextW(pane.headers[3], L"Modified");
        ApplyPaneView(paneIndex, pane.view);
        StartEnumeration(paneIndex, path);
    }
    UpdateSelectionCommands();
}

void SetActivePane(int index);

void NavigateHistory(int paneIndex, bool back) {
    Pane& pane = g_app.panes[paneIndex];
    TabState& tab = pane.tabs[pane.activeTab];
    auto& source = back ? tab.back : tab.forward;
    auto& destination = back ? tab.forward : tab.back;
    if (source.empty()) return;
    destination.push_back(tab.path);
    std::wstring path = source.back();
    source.pop_back();
    Navigate(paneIndex, path, false);
}

LRESULT CALLBACK TabSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR paneValue) {
    const int paneIndex = static_cast<int>(paneValue);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, g_app.surfaceBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_app.colors.text);
        const int selectedIndex = TabCtrl_GetCurSel(window);
        const int count = TabCtrl_GetItemCount(window);
        for (int index = 0; index < count; ++index) {
            RECT itemRect{};
            if (!TabCtrl_GetItemRect(window, index, &itemRect)) continue;
            const bool selected = index == selectedIndex;
            HBRUSH itemBrush = CreateSolidBrush(selected ? g_app.colors.buttonPressed : g_app.colors.surface);
            FillRect(dc, &itemRect, itemBrush);
            DeleteObject(itemBrush);
            if (selected) {
                RECT line{itemRect.left + Scale(7), itemRect.bottom - Scale(3), itemRect.right - Scale(7), itemRect.bottom - Scale(1)};
                HBRUSH lineBrush = CreateSolidBrush(g_app.colors.text);
                FillRect(dc, &line, lineBrush);
                DeleteObject(lineBrush);
            }
            wchar_t text[256]{};
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = text;
            item.cchTextMax = ARRAYSIZE(text);
            TabCtrl_GetItem(window, index, &item);
            RECT textRect = itemRect;
            InflateRect(&textRect, -Scale(8), 0);
            SelectObject(dc, selected ? g_app.fontBold : g_app.font);
            DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        TCHITTESTINFO hit{};
        hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int index = TabCtrl_HitTest(window, &hit);
        if (index >= 0 && paneIndex >= 0 && paneIndex < 2) {
            Pane& pane = g_app.panes[paneIndex];
            pane.activeTab = index;
            TabCtrl_SetCurSel(window, index);
            SetActivePane(paneIndex);
            Navigate(paneIndex, pane.tabs[index].path, false);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
    }
    if (message == WM_RBUTTONUP) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window, &point);
        SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
                     MAKELPARAM(point.x, point.y));
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TabSubclassProc, 1);
    return DefSubclassProc(window, message, wParam, lParam);
}

bool IsPathWordCharacter(wchar_t character) {
    return iswalnum(character) != 0 || character == L'_';
}

LRESULT CALLBACK PathEditSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (wParam == VK_BACK || wParam == VK_DELETE)) {
        DWORD selectionStart = 0, selectionEnd = 0;
        SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
        if (selectionStart == selectionEnd) {
            const int length = GetWindowTextLengthW(window);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(window, text.data(), length + 1); text.resize(length);
            if (wParam == VK_BACK) {
                while (selectionStart > 0 && !IsPathWordCharacter(text[selectionStart - 1])) --selectionStart;
                while (selectionStart > 0 && IsPathWordCharacter(text[selectionStart - 1])) --selectionStart;
            } else {
                while (selectionEnd < static_cast<DWORD>(text.size()) && IsPathWordCharacter(text[selectionEnd])) ++selectionEnd;
                while (selectionEnd < static_cast<DWORD>(text.size()) && !IsPathWordCharacter(text[selectionEnd])) ++selectionEnd;
            }
            SendMessageW(window, EM_SETSEL, selectionStart, selectionEnd);
        }
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
        return 0;
    }
    if (message == WM_CHAR && wParam == 0x7F) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, PathEditSubclassProc, 3);
    return DefSubclassProc(window, message, wParam, lParam);
}

std::vector<std::wstring> SelectedPaths(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    std::vector<std::wstring> paths;
    if (pane.mode != PaneMode::Filesystem) return paths;
    int index = -1;
    while ((index = ListView_GetNextItem(pane.list, index, LVNI_SELECTED)) != -1) {
        if (index < static_cast<int>(pane.items.size()) && pane.items[index].IsActionable() &&
            !pane.items[index].fullPath.empty()) paths.push_back(pane.items[index].fullPath);
    }
    return paths;
}

void ApplyPaneFilter(int paneIndex, bool preserveSelection) {
    Pane& pane = g_app.panes[paneIndex];
    const std::vector<std::wstring> selected = preserveSelection ? SelectedPaths(paneIndex) : std::vector<std::wstring>{};
    if (pane.timeline) {
        RebuildTimelineRows(pane, selected);
    } else if (pane.mode == PaneMode::Filesystem || pane.mode == PaneMode::Archive) {
        pane.items.clear();
        for (const FileItem& item : pane.sourceItems) {
            if (pane.filterText.empty() || StrStrIW(item.name.c_str(), pane.filterText.c_str()) ||
                StrStrIW(item.extension.c_str(), pane.filterText.c_str()) || StrStrIW(item.type.c_str(), pane.filterText.c_str()))
                pane.items.push_back(item);
        }
        ListView_SetItemCountEx(pane.list, static_cast<int>(pane.items.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        for (int row = 0; row < static_cast<int>(pane.items.size()); ++row) {
            if (std::any_of(selected.begin(), selected.end(), [&](const std::wstring& path) { return SamePath(path, pane.items[row].fullPath); }))
                ListView_SetItemState(pane.list, row, LVIS_SELECTED, LVIS_SELECTED);
        }
        InvalidateRect(pane.list, nullptr, FALSE);
    } else {
        return;
    }
    const size_t total = pane.timeline ? pane.timelineItems.size() : pane.sourceItems.size();
    size_t visible = 0;
    for (const FileItem& item : pane.items) if (item.IsActionable()) ++visible;
    const std::wstring status = pane.filterText.empty()
        ? std::to_wstring(total) + L" items"
        : std::to_wstring(visible) + L" of " + std::to_wstring(total) + L" items";
    SetWindowTextW(pane.status, status.c_str());
    if (paneIndex == g_app.activePane) UpdateSelectionCommands();
}

bool IsZipFilePath(const std::wstring& path) {
    if (_wcsicmp(PathFindExtensionW(path.c_str()), L".zip") != 0) return false;
    const DWORD attributes = GetFileAttributesW(ExtendedPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring SelectedZipPath(int paneIndex) {
    const std::vector<std::wstring> paths = SelectedPaths(paneIndex);
    return paths.size() == 1 && IsZipFilePath(paths.front()) ? paths.front() : std::wstring{};
}

std::vector<std::wstring> SelectedArchiveEntries(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    std::vector<std::wstring> entries;
    if (pane.mode != PaneMode::Archive) return entries;
    int index = -1;
    while ((index = ListView_GetNextItem(pane.list, index, LVNI_SELECTED)) != -1)
        if (index < static_cast<int>(pane.items.size()) && pane.items[index].IsActionable())
            entries.push_back(pane.items[index].archiveInternalPath);
    return entries;
}

bool IsCutPath(const std::wstring& path) {
    for (const auto& cutPath : g_app.cutPaths)
        if (_wcsicmp(cutPath.c_str(), path.c_str()) == 0) return true;
    return false;
}

void ApplyCutVisuals() {
    for (Pane& pane : g_app.panes) {
        if (!pane.list) continue;
        for (size_t index = 0; index < pane.items.size(); ++index) {
            if (!pane.items[index].IsActionable()) continue;
            ListView_SetItemState(pane.list, static_cast<int>(index),
                                  IsCutPath(pane.items[index].fullPath) ? LVIS_CUT : 0, LVIS_CUT);
        }
        InvalidateRect(pane.list, nullptr, TRUE);
    }
}

void SetCutPaths(const std::vector<std::wstring>& paths) {
    g_app.cutPaths.clear();
    g_app.cutPaths.insert(paths.begin(), paths.end());
    ApplyCutVisuals();
}

void SyncCutVisualsFromClipboard() {
    std::vector<std::wstring> paths;
    if (!OpenClipboard(g_app.window)) return;
    bool move = false;
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL effectMemory = static_cast<HGLOBAL>(GetClipboardData(effectFormat));
    if (effectMemory) {
        const auto* effect = static_cast<const DWORD*>(GlobalLock(effectMemory));
        if (effect) { move = (*effect & DROPEFFECT_MOVE) != 0; GlobalUnlock(effectMemory); }
    }
    if (move) {
        HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (drop) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1); path.resize(length);
                paths.push_back(std::move(path));
            }
        }
    }
    CloseClipboard();
    SetCutPaths(paths);
}

bool ActiveFolderAcceptsPaste() {
    const Pane& pane = g_app.panes[g_app.activePane];
    if (pane.tabs.empty() || pane.activeTab < 0 || pane.activeTab >= static_cast<int>(pane.tabs.size())) return false;
    const DWORD attributes = GetFileAttributesW(ExtendedPath(pane.tabs[pane.activeTab].path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void UpdateSelectionCommands() {
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode == PaneMode::RecycleBin) {
        const int selected = ListView_GetSelectedCount(pane.list);
        const bool hasSelection = selected > 0;
        EnableWindow(g_app.selectionButtons[0], hasSelection);
        EnableWindow(g_app.selectionButtons[4], hasSelection);
        EnableWindow(g_app.selectionButtons[5], hasSelection); EnableWindow(g_app.selectionButtons[6], FALSE);
        for (size_t index = 0; index < g_app.selectionButtons.size(); ++index)
            ShowWindow(g_app.selectionButtons[index], hasSelection && (index == 0 || index == 4 || index == 5) ? SW_SHOW : SW_HIDE);
        g_app.selectionActionsVisible = hasSelection;
        LayoutWindow(); return;
    }
    if (pane.mode == PaneMode::Archive) {
        const bool hasSelection = !SelectedArchiveEntries(g_app.activePane).empty();
        EnableWindow(g_app.selectionButtons[0], FALSE); EnableWindow(g_app.selectionButtons[1], hasSelection);
        EnableWindow(g_app.selectionButtons[2], FALSE); EnableWindow(g_app.selectionButtons[3], FALSE);
        EnableWindow(g_app.selectionButtons[4], FALSE); EnableWindow(g_app.selectionButtons[5], hasSelection);
        EnableWindow(g_app.selectionButtons[6], TRUE);
        for (size_t index = 0; index < g_app.selectionButtons.size(); ++index)
            ShowWindow(g_app.selectionButtons[index], hasSelection && (index == 1 || index == 5 || index == 6) ? SW_SHOW : SW_HIDE);
        g_app.selectionActionsVisible = hasSelection; LayoutWindow(); return;
    }
    if (pane.mode != PaneMode::Filesystem) {
        g_app.selectionActionsVisible = false;
        for (HWND button : g_app.selectionButtons) ShowWindow(button, SW_HIDE);
        LayoutWindow(); return;
    }
    const size_t selected = SelectedPaths(g_app.activePane).size();
    const bool hasSelection = selected != 0;
    EnableWindow(g_app.selectionButtons[0], hasSelection);                    // Cut
    EnableWindow(g_app.selectionButtons[1], hasSelection);                    // Copy
    EnableWindow(g_app.selectionButtons[2], IsClipboardFormatAvailable(CF_HDROP) && ActiveFolderAcceptsPaste());
    EnableWindow(g_app.selectionButtons[3], selected == 1);                   // Rename
    EnableWindow(g_app.selectionButtons[4], hasSelection);                    // Delete
    EnableWindow(g_app.selectionButtons[5], hasSelection);                    // More
    EnableWindow(g_app.selectionButtons[6], !SelectedZipPath(g_app.activePane).empty()); // Extract all
    for (HWND button : g_app.selectionButtons) if (button) InvalidateRect(button, nullptr, TRUE);
    g_app.selectionActionsVisible = hasSelection;
    LayoutWindow();
}

bool BuildShellSelection(const std::vector<std::wstring>& paths, ShellSelection& selection) {
    if (paths.empty()) return false;
    selection.absolutePidls.reserve(paths.size());
    selection.childPidls.reserve(paths.size());
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE absolute = nullptr;
        SFGAOF attributes = 0;
        if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, &attributes)) || !absolute) return false;
        PIDLIST_ABSOLUTE parentPidl = ILCloneFull(absolute);
        if (!parentPidl || !ILRemoveLastID(parentPidl)) {
            if (parentPidl) ILFree(parentPidl);
            CoTaskMemFree(absolute);
            return false;
        }
        if (!selection.parentPidl) selection.parentPidl = parentPidl;
        else {
            const bool sameParent = ILIsEqual(selection.parentPidl, parentPidl) != FALSE;
            ILFree(parentPidl);
            if (!sameParent) { CoTaskMemFree(absolute); return false; }
        }
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(absolute, IID_PPV_ARGS(&parent), &child)) || !child) {
            CoTaskMemFree(absolute);
            return false;
        }
        if (!selection.parent) selection.parent = parent;
        selection.absolutePidls.push_back(absolute);
        selection.childPidls.push_back(child);
    }
    return selection.parent && selection.childPidls.size() == paths.size();
}

void ShowError(HRESULT result, const wchar_t* action);
std::shared_ptr<OperationJob> EnqueueOperation(UINT command, std::vector<std::wstring> sources,
                                               std::wstring destination, std::wstring newName);
void RefreshAll();

void PutSelectionOnClipboard(bool cut) {
    std::vector<std::wstring> paths = SelectedPaths(g_app.activePane);
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode == PaneMode::Archive) {
        const std::vector<std::wstring> entries = SelectedArchiveEntries(g_app.activePane);
        const std::wstring cache = ArchiveCacheFolder();
        if (entries.empty() || cache.empty()) return;
        const HRESULT result = ExtractZipEntries(pane.archivePath, entries, cache);
        if (FAILED(result)) { ShowError(result, L"Copy from archive"); return; }
        for (const std::wstring& entry : entries) paths.push_back(ArchiveExtractedPath(cache, entry));
        cut = false;
    }
    if (paths.empty()) return;
    size_t characters = 1;
    for (const auto& path : paths) characters += path.size() + 1;
    const size_t bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
    HGLOBAL dropMemory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!dropMemory) return;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(dropMemory));
    if (!drop) { GlobalFree(dropMemory); return; }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* output = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    for (const auto& path : paths) {
        memcpy(output, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        output += path.size() + 1;
    }
    *output = L'\0';
    GlobalUnlock(dropMemory);
    if (!OpenClipboard(g_app.window)) { GlobalFree(dropMemory); return; }
    EmptyClipboard();
    const bool dropStored = SetClipboardData(CF_HDROP, dropMemory) != nullptr;
    if (!dropStored) GlobalFree(dropMemory);
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL effectMemory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effectMemory) {
        auto* effect = static_cast<DWORD*>(GlobalLock(effectMemory));
        if (effect) { *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY; GlobalUnlock(effectMemory); }
        if (!effect || !SetClipboardData(effectFormat, effectMemory)) GlobalFree(effectMemory);
    }
    CloseClipboard();
    if (dropStored) SetCutPaths(cut ? paths : std::vector<std::wstring>{});
}

void PasteClipboard() {
    if (!OpenClipboard(g_app.window)) return;
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    std::vector<std::wstring> paths;
    if (drop) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            path.resize(length);
            paths.push_back(std::move(path));
        }
    }
    bool move = false;
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL effectMemory = static_cast<HGLOBAL>(GetClipboardData(effectFormat));
    if (effectMemory) {
        const auto* effect = static_cast<const DWORD*>(GlobalLock(effectMemory));
        if (effect) { move = (*effect & DROPEFFECT_MOVE) != 0; GlobalUnlock(effectMemory); }
    }
    CloseClipboard();
    if (paths.empty()) return;
    Pane& pane = g_app.panes[g_app.activePane];
    EnqueueOperation(move ? ID_MOVE : ID_COPY, std::move(paths), pane.tabs[pane.activeTab].path, {});
}

void ShowError(HRESULT result, const wchar_t* action) {
    if (SUCCEEDED(result)) return;
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = action;
    text += L" failed.";
    if (message) { text += L"\n\n"; text += message; LocalFree(message); }
    MessageBoxW(g_app.window, text.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

void SaveJobJournal();
void PumpOperationQueue();
ULONGLONG CurrentFileTimeValue();

std::wstring OperationResourceKey(const std::wstring& input) {
    if (input.rfind(L"\\\\", 0) == 0) {
        const size_t serverEnd = input.find(L'\\', 2);
        const size_t shareEnd = serverEnd == std::wstring::npos ? std::wstring::npos : input.find(L'\\', serverEnd + 1);
        std::wstring key = shareEnd == std::wstring::npos ? input : input.substr(0, shareEnd);
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        return key;
    }
    wchar_t mount[MAX_PATH]{};
    if (!GetVolumePathNameW(input.c_str(), mount, ARRAYSIZE(mount))) {
        std::wstring key = input.substr(0, (std::min<size_t>)(3, input.size()));
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        return key;
    }
    wchar_t volume[MAX_PATH]{};
    std::wstring key = GetVolumeNameForVolumeMountPointW(mount, volume, ARRAYSIZE(volume)) ? volume : mount;
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    return key;
}

std::wstring ShellItemPath(IShellItem* item) {
    if (!item) return {};
    PWSTR value = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) || !value) return {};
    std::wstring result(value); CoTaskMemFree(value); return result;
}

class OperationProgressSink final : public IFileOperationProgressSink {
public:
    explicit OperationProgressSink(std::shared_ptr<OperationJob> job) : job_(std::move(job)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IFileOperationProgressSink) {
            *object = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return Gate(); }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override { job_->result = result; Notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem* item, LPCWSTR) override { return BeginItem(item); }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) override { return EndItem(result); }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR) override { return BeginItem(item); }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) override { return EndItem(result); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR) override { return BeginItem(item); }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) override { return EndItem(result); }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem* item) override { return BeginItem(item); }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem*) override { return EndItem(result); }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem* folder, LPCWSTR name) override {
        { std::lock_guard lock(job_->textMutex); job_->currentItem = JoinPath(ShellItemPath(folder), name ? name : L""); }
        Notify(); return Gate();
    }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT result, IShellItem*) override { return EndItem(result); }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT total, UINT done) override {
        job_->progressTotal = total; job_->progressDone = done; Notify(); return Gate();
    }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
private:
    HRESULT BeginItem(IShellItem* item) {
        { std::lock_guard lock(job_->textMutex); job_->currentItem = ShellItemPath(item); }
        Notify(); return Gate();
    }
    HRESULT EndItem(HRESULT result) { if (FAILED(result)) job_->result = result; Notify(); return Gate(); }
    HRESULT Gate() {
        if (job_->cancelRequested) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (job_->pauseRequested) {
            job_->state = JobState::Paused; Notify();
            std::unique_lock lock(job_->controlMutex);
            job_->controlChanged.wait(lock, [&] { return !job_->pauseRequested.load() || job_->cancelRequested.load(); });
            if (job_->cancelRequested) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            job_->state = JobState::Running; Notify();
        }
        return S_OK;
    }
    void Notify() { if (g_app.window) PostMessageW(g_app.window, WM_APP_JOB_UPDATE, 0, 0); }
    std::atomic_ulong references_{1};
    std::shared_ptr<OperationJob> job_;
};

HRESULT ExecuteOperationJob(const std::shared_ptr<OperationJob>& job) {
    ComPtr<IFileOperation> operation;
    HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (FAILED(result)) return result;
    operation->SetOwnerWindow(g_app.window);
    DWORD flags = FOF_NOCONFIRMMKDIR | FOF_SILENT | FOFX_SHOWELEVATIONPROMPT;
    if (job->kind == JobKind::Recycle) flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
    if (job->conflict == ConflictPolicy::KeepBoth) flags |= FOF_RENAMEONCOLLISION;
    if (job->conflict == ConflictPolicy::Replace) flags |= FOF_NOCONFIRMATION;
    result = operation->SetOperationFlags(flags);
    if (FAILED(result)) return result;

    auto* sink = new (std::nothrow) OperationProgressSink(job);
    if (!sink) return E_OUTOFMEMORY;
    DWORD cookie = 0;
    result = operation->Advise(sink, &cookie);
    sink->Release();
    if (FAILED(result)) return result;

    ComPtr<IShellItem> destinationItem;
    if (!job->destination.empty()) {
        result = SHCreateItemFromParsingName(job->destination.c_str(), nullptr, IID_PPV_ARGS(&destinationItem));
        if (FAILED(result)) return result;
    }

    if (job->kind == JobKind::NewFolder) {
        result = operation->NewItem(destinationItem.Get(), FILE_ATTRIBUTE_DIRECTORY, job->newName.c_str(), nullptr, nullptr);
    } else {
        for (const auto& path : job->sources) {
            ComPtr<IShellItem> item;
            result = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
            if (FAILED(result)) return result;
            if (job->kind == JobKind::Copy) result = operation->CopyItem(item.Get(), destinationItem.Get(), nullptr, nullptr);
            else if (job->kind == JobKind::Move) result = operation->MoveItem(item.Get(), destinationItem.Get(), nullptr, nullptr);
            else if (job->kind == JobKind::Rename) result = operation->RenameItem(item.Get(), job->newName.c_str(), nullptr);
            else result = operation->DeleteItem(item.Get(), nullptr);
            if (FAILED(result)) return result;
        }
    }
    if (SUCCEEDED(result)) result = operation->PerformOperations();
    if (SUCCEEDED(result)) {
        BOOL aborted = FALSE;
        if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    operation->Unadvise(cookie);
    return result;
}

JobKind CommandJobKind(UINT command) {
    if (command == ID_COPY) return JobKind::Copy;
    if (command == ID_MOVE) return JobKind::Move;
    if (command == ID_DELETE) return JobKind::Recycle;
    if (command == ID_DELETE_PERMANENT) return JobKind::DeletePermanent;
    if (command == ID_RENAME) return JobKind::Rename;
    return JobKind::NewFolder;
}

bool ResolveTopLevelConflicts(OperationJob& job) {
    if ((job.kind != JobKind::Copy && job.kind != JobKind::Move) || job.destination.empty()) return true;
    std::vector<std::wstring> collisions;
    for (const auto& source : job.sources) {
        const wchar_t* name = PathFindFileNameW(source.c_str());
        if (name && GetFileAttributesW(ExtendedPath(JoinPath(job.destination, name)).c_str()) != INVALID_FILE_ATTRIBUTES) collisions.push_back(source);
    }
    if (collisions.empty()) return true;
    const std::wstring firstName = PathFindFileNameW(collisions.front().c_str());
    const std::wstring content = L"“" + firstName + L"” already exists in the destination.\n\nChoose a policy for all " +
                                 std::to_wstring(collisions.size()) + L" top-level conflict" + (collisions.size() == 1 ? L"." : L"s.");
    const TASKDIALOG_BUTTON buttons[] = {{1001, L"Replace"}, {1002, L"Keep both"}, {1003, L"Skip conflicts"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = g_app.window; config.hInstance = g_app.instance;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"File conflict"; config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"Files with the same name were found"; config.pszContent = content.c_str();
    config.cButtons = ARRAYSIZE(buttons); config.pButtons = buttons; config.nDefaultButton = 1003;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
        const int fallback = MessageBoxW(g_app.window,
            (content + L"\n\nYes: replace\nNo: keep both\nCancel: stop").c_str(), L"File conflict",
            MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
        choice = fallback == IDYES ? 1001 : fallback == IDNO ? 1002 : IDCANCEL;
    }
    if (choice == IDCANCEL) return false;
    if (choice == 1001) job.conflict = ConflictPolicy::Replace;
    else if (choice == 1002) job.conflict = ConflictPolicy::KeepBoth;
    else {
        job.conflict = ConflictPolicy::Skip;
        std::erase_if(job.sources, [&](const std::wstring& source) {
            const wchar_t* name = PathFindFileNameW(source.c_str());
            return name && GetFileAttributesW(ExtendedPath(JoinPath(job.destination, name)).c_str()) != INVALID_FILE_ATTRIBUTES;
        });
    }
    return !job.sources.empty();
}

std::shared_ptr<OperationJob> EnqueueOperation(UINT command, std::vector<std::wstring> sources,
                                               std::wstring destination = {}, std::wstring newName = {}) {
    auto job = std::make_shared<OperationJob>();
    job->kind = CommandJobKind(command); job->sources = std::move(sources);
    job->destination = std::move(destination); job->newName = std::move(newName);
    if (!ResolveTopLevelConflicts(*job)) return nullptr;
    job->created = CurrentFileTimeValue();
    for (const auto& source : job->sources) job->resourceKeys.push_back(OperationResourceKey(source));
    if (!job->destination.empty()) job->resourceKeys.push_back(OperationResourceKey(job->destination));
    std::sort(job->resourceKeys.begin(), job->resourceKeys.end());
    job->resourceKeys.erase(std::unique(job->resourceKeys.begin(), job->resourceKeys.end()), job->resourceKeys.end());
    {
        std::lock_guard lock(g_app.operations.mutex);
        job->id = g_app.operations.nextId++;
        g_app.operations.jobs.push_back(job);
    }
    SaveJobJournal(); PumpOperationQueue(); LayoutWindow();
    return job;
}

void RunOperationWorker(std::shared_ptr<OperationJob> job) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    job->state = JobState::Running;
    HRESULT result = SUCCEEDED(initialized) ? ExecuteOperationJob(job) : initialized;
    if (SUCCEEDED(initialized)) CoUninitialize();
    job->result = result;
    if (job->cancelRequested || result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || result == E_ABORT) job->state = JobState::Failed;
    else job->state = SUCCEEDED(result) ? JobState::Completed : JobState::Failed;
    auto* completed = new (std::nothrow) std::shared_ptr<OperationJob>(job);
    if (completed && !PostMessageW(g_app.window, WM_APP_JOB_DONE, 0, reinterpret_cast<LPARAM>(completed))) delete completed;
}

void PumpOperationQueue() {
    std::vector<std::shared_ptr<OperationJob>> starting;
    {
        std::lock_guard lock(g_app.operations.mutex);
        for (const auto& job : g_app.operations.jobs) {
            if (g_app.operations.activeCount >= 3) break;
            if (job->state != JobState::Queued) continue;
            bool blocked = false;
            for (const auto& root : job->resourceKeys) if (g_app.operations.activeRoots.contains(root)) { blocked = true; break; }
            if (blocked) continue;
            job->state = JobState::Running;
            for (const auto& root : job->resourceKeys) g_app.operations.activeRoots.insert(root);
            ++g_app.operations.activeCount; starting.push_back(job);
        }
    }
    if (!starting.empty()) SaveJobJournal();
    for (auto& job : starting) std::thread(RunOperationWorker, job).detach();
}

void RefreshAll() {
    for (int i = 0; i < 2; ++i) {
        Pane& pane = g_app.panes[i];
        if (pane.tabs.empty()) continue;
        if (pane.mode == PaneMode::DriveOverview) StartDriveOverview(i);
        else if (pane.mode == PaneMode::RecycleBin) LoadRecycleBin(i);
        else if (pane.mode == PaneMode::Archive) StartArchiveEnumeration(i, pane.archivePath, pane.archiveInternalPath);
        else StartEnumeration(i, pane.tabs[pane.activeTab].path);
    }
}

bool ExtractDropPaths(IDataObject* dataObject, std::vector<std::wstring>& paths) {
    if (!dataObject) return false;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&format, &medium))) return false;
    HDROP drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
            path.resize(length);
            paths.push_back(std::move(path));
        }
    }
    ReleaseStgMedium(&medium);
    return !paths.empty();
}

DWORD ChooseDropEffect(DWORD keyState, DWORD allowed, const std::vector<std::wstring>& paths,
                       const std::wstring& destination) {
    DWORD requested = DROPEFFECT_NONE;
    if ((keyState & MK_CONTROL) != 0) requested = DROPEFFECT_COPY;
    else if ((keyState & MK_SHIFT) != 0) requested = DROPEFFECT_MOVE;
    else if (g_app.preferences.dragMoveSameDrive && !paths.empty() && PathIsSameRootW(paths.front().c_str(), destination.c_str())) requested = DROPEFFECT_MOVE;
    else requested = DROPEFFECT_COPY;
    if ((allowed & requested) != 0) return requested;
    if ((allowed & DROPEFFECT_COPY) != 0) return DROPEFFECT_COPY;
    if ((allowed & DROPEFFECT_MOVE) != 0) return DROPEFFECT_MOVE;
    return DROPEFFECT_NONE;
}

class FileDropTarget final : public IDropTarget {
public:
    explicit FileDropTarget(int paneIndex) : paneIndex_(paneIndex) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *value = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --references_;
        if (value == 0) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        paths_.clear();
        if (!ExtractDropPaths(dataObject, paths_)) { *effect = DROPEFFECT_NONE; return S_OK; }
        allowedEffects_ = *effect;
        UpdateHighlight(point);
        *effect = ChooseDropEffect(keyState, allowedEffects_, paths_, DestinationAt(point));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        UpdateHighlight(point);
        *effect = paths_.empty() ? DROPEFFECT_NONE : ChooseDropEffect(keyState, allowedEffects_, paths_, DestinationAt(point));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { ClearHighlight(); paths_.clear(); allowedEffects_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override {
        if (!effect) return E_INVALIDARG;
        std::vector<std::wstring> paths;
        if (!ExtractDropPaths(dataObject, paths)) { *effect = DROPEFFECT_NONE; return S_OK; }
        const std::wstring destination = DestinationAt(point);
        const DWORD chosen = ChooseDropEffect(keyState, allowedEffects_ ? allowedEffects_ : *effect, paths, destination);
        ClearHighlight();
        if (chosen == DROPEFFECT_NONE || destination.empty()) { *effect = DROPEFFECT_NONE; return S_OK; }
        SetActivePane(paneIndex_);
        EnqueueOperation(chosen == DROPEFFECT_MOVE ? ID_MOVE : ID_COPY, std::move(paths), destination, {});
        *effect = chosen;
        paths_.clear();
        allowedEffects_ = 0;
        return S_OK;
    }

private:
    int HitDirectory(POINTL point) const {
        const Pane& pane = g_app.panes[paneIndex_];
        POINT client{point.x, point.y};
        ScreenToClient(pane.list, &client);
        LVHITTESTINFO hit{};
        hit.pt = client;
        const int item = ListView_HitTest(pane.list, &hit);
        return item >= 0 && item < static_cast<int>(pane.items.size()) && pane.items[item].IsDirectory() ? item : -1;
    }
    void ClearHighlight() {
        if (highlightedItem_ < 0) return;
        Pane& pane = g_app.panes[paneIndex_];
        ListView_SetItemState(pane.list, highlightedItem_, 0, LVIS_DROPHILITED);
        highlightedItem_ = -1;
    }
    void UpdateHighlight(POINTL point) {
        const int item = HitDirectory(point);
        if (item == highlightedItem_) return;
        ClearHighlight();
        if (item >= 0) {
            ListView_SetItemState(g_app.panes[paneIndex_].list, item, LVIS_DROPHILITED, LVIS_DROPHILITED);
            highlightedItem_ = item;
        }
    }
    std::wstring DestinationAt(POINTL point) const {
        if (paneIndex_ < 0 || paneIndex_ > 1) return {};
        const Pane& pane = g_app.panes[paneIndex_];
        if (pane.tabs.empty() || pane.mode != PaneMode::Filesystem) return {};
        const int item = HitDirectory(point);
        if (item >= 0 && item < static_cast<int>(pane.items.size()) && pane.items[item].IsDirectory()) {
            return pane.items[item].fullPath;
        }
        return pane.tabs[pane.activeTab].path;
    }

    std::atomic_ulong references_{1};
    int paneIndex_ = 0;
    int highlightedItem_ = -1;
    DWORD allowedEffects_ = 0;
    std::vector<std::wstring> paths_;
};

class FileDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *value = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --references_;
        if (value == 0) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
private:
    std::atomic_ulong references_{1};
};

void StartShellDrag(int paneIndex) {
    std::vector<std::wstring> paths = SelectedPaths(paneIndex);
    Pane& pane = g_app.panes[paneIndex];
    if (pane.mode == PaneMode::Archive) {
        const std::vector<std::wstring> entries = SelectedArchiveEntries(paneIndex);
        if (entries.empty()) return;
        const std::wstring cache = ArchiveCacheFolder(); if (cache.empty()) return;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        const HRESULT extracted = ExtractZipEntries(pane.archivePath, entries, cache);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (FAILED(extracted)) { ShowError(extracted, L"Prepare archive drag"); return; }
        for (const std::wstring& entry : entries) paths.push_back(ArchiveExtractedPath(cache, entry));
    }
    ShellSelection selection;
    if (!BuildShellSelection(paths, selection)) return;
    ComPtr<IDataObject> dataObject;
    HRESULT result = selection.parent->GetUIObjectOf(g_app.window,
        static_cast<UINT>(selection.childPidls.size()), selection.childPidls.data(),
        IID_IDataObject, nullptr, reinterpret_cast<void**>(dataObject.GetAddressOf()));
    if (FAILED(result) || !dataObject) return;
    ComPtr<IDragSourceHelper> helper;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&helper)))) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(pane.list, &point);
        helper->InitializeFromWindow(pane.list, &point, dataObject.Get());
    }
    auto* source = new (std::nothrow) FileDropSource();
    if (!source) return;
    DWORD effect = DROPEFFECT_NONE;
    result = DoDragDrop(dataObject.Get(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    source->Release();
    if (result == DRAGDROP_S_DROP && effect == DROPEFFECT_MOVE) RefreshAll();
}

bool PaneSupportsCheckboxes(const Pane& pane) {
    return pane.mode == PaneMode::Filesystem || pane.mode == PaneMode::RecycleBin || pane.mode == PaneMode::Archive;
}

void InvalidateListItem(HWND list, int item) {
    if (!list || item < 0) return;
    RECT bounds{};
    if (ListView_GetItemRect(list, item, &bounds, LVIR_BOUNDS)) InvalidateRect(list, &bounds, FALSE);
}

bool IsCheckboxHit(const Pane& pane, const LVHITTESTINFO& hit) {
    if (hit.iItem < 0 || !PaneSupportsCheckboxes(pane)) return false;
    if (hit.iItem >= static_cast<int>(pane.items.size()) || !pane.items[hit.iItem].IsActionable()) return false;
    RECT row{};
    if (!ListView_GetItemRect(pane.list, hit.iItem, &row, LVIR_BOUNDS)) return false;
    const int checkboxSize = (std::max)(Scale(18), 16);
    const bool iconLayout = pane.mode == PaneMode::Filesystem && pane.view <= FileViewMode::SmallIcons;
    const int checkboxY = iconLayout ? row.top + Scale(3)
                                     : row.top + ((row.bottom - row.top) - checkboxSize) / 2;
    RECT checkbox{row.left + Scale(3), checkboxY,
                  row.left + Scale(3) + checkboxSize, checkboxY + checkboxSize};
    return PtInRect(&checkbox, hit.pt) != FALSE;
}

LRESULT CALLBACK ListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR paneValue) {
    const int paneIndex = static_cast<int>(paneValue);
    if (paneIndex < 0 || paneIndex > 1) return DefSubclassProc(window, message, wParam, lParam);
    Pane& pane = g_app.panes[paneIndex];
    if (message == WM_STYLECHANGING && wParam == GWL_STYLE) {
        reinterpret_cast<STYLESTRUCT*>(lParam)->styleNew &= ~WS_HSCROLL;
    }
    if (message == WM_HSCROLL || message == WM_MOUSEHWHEEL) {
        ShowScrollBar(window, SB_HORZ, FALSE);
        return 0;
    }
    if (message == WM_SIZE) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        ShowScrollBar(window, SB_HORZ, FALSE);
        return result;
    }
    if (message == WM_LBUTTONDOWN) {
        LVHITTESTINFO hit{};
        hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ListView_HitTest(window, &hit);
        if (IsCheckboxHit(pane, hit)) {
            SetActivePane(paneIndex);
            const bool selected = (ListView_GetItemState(window, hit.iItem, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            ListView_SetItemState(window, hit.iItem, selected ? 0 : LVIS_SELECTED, LVIS_SELECTED);
            ListView_SetItemState(window, hit.iItem, LVIS_FOCUSED, LVIS_FOCUSED);
            InvalidateListItem(window, hit.iItem);
            UpdateSelectionCommands();
            pane.dragCandidate = false;
            return 0;
        }
    } else if (message == WM_MOUSEMOVE) {
        if (PaneSupportsCheckboxes(pane)) {
            LVHITTESTINFO hover{};
            hover.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ListView_HitTest(window, &hover);
            const int hovered = hover.iItem;
            if (hovered != pane.hoveredCheckboxItem) {
                const int previous = pane.hoveredCheckboxItem;
                pane.hoveredCheckboxItem = hovered;
                InvalidateListItem(window, previous);
                InvalidateListItem(window, hovered);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
        }
    } else if (message == WM_MOUSELEAVE) {
        const int previous = pane.hoveredCheckboxItem;
        pane.hoveredCheckboxItem = -1;
        InvalidateListItem(window, previous);
    } else if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        pane.dragCandidate = false;
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ListSubclassProc, 2);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK SidebarScrollSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_STYLECHANGING && wParam == GWL_STYLE) {
        reinterpret_cast<STYLESTRUCT*>(lParam)->styleNew &= ~WS_HSCROLL;
    }
    if (message == WM_HSCROLL || message == WM_MOUSEHWHEEL) {
        ShowScrollBar(window, SB_HORZ, FALSE);
        return 0;
    }
    if (message == WM_SIZE) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        ShowScrollBar(window, SB_HORZ, FALSE);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SidebarScrollSubclassProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

void CopyMoveDelete(UINT command) {
    const int source = g_app.activePane;
    std::vector<std::wstring> paths = SelectedPaths(source);
    if (paths.empty()) return;
    std::wstring destination;
    if (command == ID_COPY || command == ID_MOVE) destination = g_app.panes[1 - source].tabs[g_app.panes[1 - source].activeTab].path;
    if (command == ID_DELETE_PERMANENT && g_app.preferences.confirmPermanentDelete) {
        if (MessageBoxW(g_app.window, L"Permanently delete selected items? This cannot be undone.", kAppName,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    EnqueueOperation(command, std::move(paths), std::move(destination), {});
}

void CreateFolder() {
    Pane& pane = g_app.panes[g_app.activePane];
    const std::wstring parent = pane.tabs[pane.activeTab].path;
    std::wstring name = L"New folder";
    for (int suffix = 2; GetFileAttributesW(ExtendedPath(JoinPath(parent, name)).c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix) {
        name = L"New folder (" + std::to_wstring(suffix) + L")";
    }
    pane.pendingRenamePath = JoinPath(parent, name);
    EnqueueOperation(ID_NEW_FOLDER, {}, parent, name);
}

bool ReadRegistryBytes(const std::wstring& keyPath, const wchar_t* valueName, DWORD& type,
                       std::vector<BYTE>& bytes) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CLASSES_ROOT, keyPath.c_str(), valueName, RRF_RT_ANY, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    bytes.assign(size ? size : sizeof(wchar_t), 0);
    return RegGetValueW(HKEY_CLASSES_ROOT, keyPath.c_str(), valueName, RRF_RT_ANY,
                        &type, bytes.data(), &size) == ERROR_SUCCESS;
}

std::wstring RegistryString(const std::wstring& keyPath, const wchar_t* valueName = nullptr) {
    DWORD type = 0; std::vector<BYTE> bytes;
    if (!ReadRegistryBytes(keyPath, valueName, type, bytes) ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes.size() < sizeof(wchar_t)) return {};
    bytes.resize(bytes.size() + sizeof(wchar_t), 0);
    std::wstring value(reinterpret_cast<const wchar_t*>(bytes.data()));
    if (type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (needed) {
            std::wstring expanded(needed, L'\0');
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed)) {
                expanded.resize(needed - 1); value = std::move(expanded);
            }
        }
    }
    return value;
}

std::wstring ResolveShellNewTemplateFile(const std::wstring& value) {
    if (value.empty()) return {};
    std::wstring candidate = value;
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    wchar_t windows[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) {
        candidate = JoinPath(JoinPath(windows, L"ShellNew"), value);
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

std::wstring FriendlyTypeForExtension(const std::wstring& extension) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW((L"Files4Me" + extension).c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) && info.szTypeName[0]) return info.szTypeName;
    return extension.substr(extension.front() == L'.' ? 1 : 0) + L" file";
}

void DiscoverNewItemTemplates() {
    g_app.newItemTemplates.clear();
    HKEY classes = HKEY_CLASSES_ROOT;
    DWORD index = 0;
    wchar_t keyName[512]{};
    DWORD keyLength = ARRAYSIZE(keyName);
    while (g_app.newItemTemplates.size() < 32 &&
           RegEnumKeyExW(classes, index++, keyName, &keyLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        keyLength = ARRAYSIZE(keyName);
        std::wstring extension = keyName;
        if (extension.size() < 2 || extension.front() != L'.' || _wcsicmp(extension.c_str(), L".txt") == 0 ||
            _wcsicmp(extension.c_str(), L".lnk") == 0 || _wcsicmp(extension.c_str(), L".library-ms") == 0) continue;
        std::wstring shellNew = extension + L"\\ShellNew";
        HKEY test = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, shellNew.c_str(), 0, KEY_READ, &test) != ERROR_SUCCESS) {
            const std::wstring progId = RegistryString(extension);
            shellNew = progId.empty() ? L"" : progId + L"\\ShellNew";
            if (shellNew.empty() || RegOpenKeyExW(HKEY_CLASSES_ROOT, shellNew.c_str(), 0, KEY_READ, &test) != ERROR_SUCCESS) continue;
        }
        RegCloseKey(test);

        NewItemTemplate item; item.extension = extension;
        item.displayName = FriendlyTypeForExtension(extension);
        item.defaultBaseName = L"New " + item.displayName;
        item.icon = IconFor(L"Files4Me" + extension, FILE_ATTRIBUTE_NORMAL);
        DWORD type = 0; std::vector<BYTE> bytes;
        const std::wstring fileName = RegistryString(shellNew, L"FileName");
        if (!fileName.empty()) {
            item.source = ResolveShellNewTemplateFile(fileName);
            if (item.source.empty()) continue;
            item.kind = NewTemplateKind::TemplateFile;
        } else if (ReadRegistryBytes(shellNew, L"Data", type, bytes) &&
                   (type == REG_BINARY || type == REG_SZ || type == REG_EXPAND_SZ)) {
            item.kind = NewTemplateKind::Data; item.data = std::move(bytes);
            if ((type == REG_SZ || type == REG_EXPAND_SZ) && item.data.size() >= sizeof(wchar_t))
                item.data.resize(item.data.size() - sizeof(wchar_t));
        } else {
            item.source = RegistryString(shellNew, L"Command");
            if (!item.source.empty()) item.kind = NewTemplateKind::Command;
            else if (ReadRegistryBytes(shellNew, L"NullFile", type, bytes)) item.kind = NewTemplateKind::Empty;
            else continue;
        }
        item.command = ID_NEW_TEMPLATE_BASE + static_cast<UINT>(g_app.newItemTemplates.size());
        g_app.newItemTemplates.push_back(std::move(item));
    }
    std::sort(g_app.newItemTemplates.begin(), g_app.newItemTemplates.end(), [](const auto& left, const auto& right) {
        return StrCmpLogicalW(left.displayName.c_str(), right.displayName.c_str()) < 0;
    });
    for (size_t item = 0; item < g_app.newItemTemplates.size(); ++item)
        g_app.newItemTemplates[item].command = ID_NEW_TEMPLATE_BASE + static_cast<UINT>(item);
}

std::wstring UniqueNewItemPath(const std::wstring& parent, const std::wstring& base, const std::wstring& extension) {
    std::wstring path = JoinPath(parent, base + extension);
    for (int suffix = 2; GetFileAttributesW(ExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix)
        path = JoinPath(parent, base + L" (" + std::to_wstring(suffix) + L")" + extension);
    return path;
}

void ReplaceCommandPlaceholder(std::wstring& command, const std::wstring& token, const std::wstring& value) {
    size_t position = 0;
    while ((position = command.find(token, position)) != std::wstring::npos) {
        command.replace(position, token.size(), value); position += value.size();
    }
}

HRESULT CreateNewItemOnWorker(const NewItemTemplate& item, const std::wstring& target) {
    if (item.kind == NewTemplateKind::TemplateFile)
        return CopyFileW(item.source.c_str(), target.c_str(), TRUE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (item.kind == NewTemplateKind::Command) {
        std::wstring command = item.source;
        const std::wstring quotedTarget = L"\"" + target + L"\"";
        const bool hadPlaceholder = command.find(L"%1") != std::wstring::npos || command.find(L"%L") != std::wstring::npos ||
                                    command.find(L"%l") != std::wstring::npos;
        ReplaceCommandPlaceholder(command, L"%1", quotedTarget); ReplaceCommandPlaceholder(command, L"%L", quotedTarget);
        ReplaceCommandPlaceholder(command, L"%l", quotedTarget);
        if (!hadPlaceholder) command += L" " + quotedTarget;
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            ParentPath(target).c_str(), &startup, &process)) return HRESULT_FROM_WIN32(GetLastError());
        UniqueKernelHandle processHandle(process.hProcess), threadHandle(process.hThread);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (GetFileAttributesW(ExtendedPath(target).c_str()) != INVALID_FILE_ATTRIBUTES) return S_OK;
            if (WaitForSingleObject(processHandle.get(), 100) == WAIT_OBJECT_0) break;
        }
        return GetFileAttributesW(ExtendedPath(target).c_str()) != INVALID_FILE_ATTRIBUTES ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (item.data.size() > MAXDWORD) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    UniqueKernelHandle file(CreateFileW(ExtendedPath(target).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return HRESULT_FROM_WIN32(GetLastError());
    if (!item.data.empty()) {
        DWORD written = 0;
        if (!WriteFile(file.get(), item.data.data(), static_cast<DWORD>(item.data.size()), &written, nullptr) ||
            written != static_cast<DWORD>(item.data.size())) {
            const HRESULT result = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
            file.reset(); DeleteFileW(ExtendedPath(target).c_str()); return result;
        }
        if (!FlushFileBuffers(file.get())) return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

void CreateNewFromTemplate(UINT command) {
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode != PaneMode::Filesystem || pane.tabs.empty()) return;
    if (command == ID_CREATE_FOLDER) { CreateFolder(); return; }
    NewItemTemplate item;
    if (command == ID_CREATE_TEXT) {
        item.command = command; item.extension = L".txt"; item.displayName = L"Text Document";
        item.defaultBaseName = L"New Text Document"; item.kind = NewTemplateKind::Empty;
        item.icon = IconFor(L"Files4Me.txt", FILE_ATTRIBUTE_NORMAL);
    } else {
        const size_t index = command >= ID_NEW_TEMPLATE_BASE ? command - ID_NEW_TEMPLATE_BASE : SIZE_MAX;
        if (index >= g_app.newItemTemplates.size()) return;
        item = g_app.newItemTemplates[index];
    }
    const int paneIndex = g_app.activePane;
    const std::wstring parent = pane.tabs[pane.activeTab].path;
    const std::wstring target = UniqueNewItemPath(parent, item.defaultBaseName, item.extension);
    std::thread([paneIndex, item = std::move(item), target, window = g_app.window] {
        auto result = std::make_unique<NewItemResult>(); result->pane = paneIndex; result->path = target;
        result->result = CreateNewItemOnWorker(item, target);
        NewItemResult* raw = result.release();
        if (!PostMessageW(window, WM_APP_NEW_ITEM_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }).detach();
}

HMENU BuildNewItemsMenu() {
    DiscoverNewItemTemplates();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CREATE_FOLDER, L"Folder\tF7");
    AppendMenuW(menu, MF_STRING, ID_CREATE_TEXT, L"Text Document");
    if (!g_app.newItemTemplates.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (const NewItemTemplate& item : g_app.newItemTemplates)
        AppendMenuW(menu, MF_STRING, item.command, item.displayName.c_str());
    return menu;
}

void ShowNewMenu() {
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode != PaneMode::Filesystem) return;
    HMENU menu = BuildNewItemsMenu(); if (!menu) return;
    RECT anchor{}; GetWindowRect(g_app.commandButtons[4], &anchor);
    PrepareThemedMenu(menu); SetForegroundWindow(g_app.window);
    const UINT selected = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                               anchor.left, anchor.bottom, g_app.window);
    DestroyMenu(menu); if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
}

std::wstring ArchiveCacheFolder() {
    const std::wstring root = JoinPath(KnownFolderPath(FOLDERID_LocalAppData), L"Files4Me\\ArchiveCache");
    SHCreateDirectoryExW(g_app.window, root.c_str(), nullptr);
    const std::wstring folder = JoinPath(root, std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    return SUCCEEDED(HRESULT_FROM_WIN32(SHCreateDirectoryExW(g_app.window, folder.c_str(), nullptr))) ||
           GetLastError() == ERROR_ALREADY_EXISTS ? folder : std::wstring{};
}

std::wstring ArchiveExtractedPath(const std::wstring& destination, const std::wstring& internalPath) {
    const wchar_t* name = PathFindFileNameW(internalPath.c_str());
    return JoinPath(destination, name && *name ? name : L"Archive item");
}

void ExtractArchiveAsync(int paneIndex, std::vector<std::wstring> entries, const std::wstring& destination,
                         const std::wstring& openPath = {}, bool openInFiles4Me = false) {
    const std::wstring archive = g_app.panes[paneIndex].archivePath;
    std::thread([paneIndex, archive, entries = std::move(entries), destination, openPath, openInFiles4Me, target = g_app.window] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = std::make_unique<ArchiveExtractResult>(); result->pane = paneIndex;
        result->openPath = openPath; result->openInFiles4Me = openInFiles4Me;
        result->result = SUCCEEDED(initialized) ? ExtractZipEntries(archive, entries, destination) : initialized;
        ArchiveExtractResult* raw = result.release();
        if (!PostMessageW(target, WM_APP_ARCHIVE_EXTRACT_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}

void ExtractAllArchive() {
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode != PaneMode::Archive) return;
    std::vector<std::wstring> entries;
    if (pane.archiveInternalPath.empty()) {
        for (const FileItem& item : pane.items) if (item.IsActionable()) entries.push_back(item.archiveInternalPath);
    } else {
        std::vector<ArchiveEntryData> rootEntries;
        const HRESULT enumerated = EnumerateZipFolder(pane.archivePath, L"", rootEntries);
        if (FAILED(enumerated)) { ShowError(enumerated, L"Read archive"); return; }
        for (const ArchiveEntryData& item : rootEntries) entries.push_back(item.internalPath);
    }
    if (entries.empty()) return;
    std::wstring stem = PathFindFileNameW(pane.archivePath.c_str());
    if (const size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    std::wstring destination = JoinPath(ParentPath(pane.archivePath), stem);
    for (int suffix = 2; GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix)
        destination = JoinPath(ParentPath(pane.archivePath), stem + L" (" + std::to_wstring(suffix) + L")");
    if (SHCreateDirectoryExW(g_app.window, destination.c_str(), nullptr) != ERROR_SUCCESS) {
        ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Create extraction folder"); return;
    }
    SetWindowTextW(pane.status, L"Extracting...");
    ExtractArchiveAsync(g_app.activePane, std::move(entries), destination, destination, true);
}

void ExtractZipFile(const std::wstring& archivePath) {
    if (!IsZipFilePath(archivePath)) return;
    std::vector<ArchiveEntryData> rootItems;
    const HRESULT enumerated = EnumerateZipFolder(archivePath, L"", rootItems);
    if (FAILED(enumerated)) { ShowError(enumerated, L"Read ZIP archive"); return; }
    std::vector<std::wstring> entries;
    for (const ArchiveEntryData& item : rootItems) entries.push_back(item.internalPath);
    if (entries.empty()) return;
    std::wstring stem = PathFindFileNameW(archivePath.c_str());
    if (const size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    std::wstring destination = JoinPath(ParentPath(archivePath), stem);
    for (int suffix = 2; GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix)
        destination = JoinPath(ParentPath(archivePath), stem + L" (" + std::to_wstring(suffix) + L")");
    const int directoryResult = SHCreateDirectoryExW(g_app.window, destination.c_str(), nullptr);
    if (directoryResult != ERROR_SUCCESS && directoryResult != ERROR_ALREADY_EXISTS) {
        ShowError(HRESULT_FROM_WIN32(directoryResult), L"Create extraction folder"); return;
    }
    const int paneIndex = g_app.activePane;
    SetWindowTextW(g_app.panes[paneIndex].status, L"Extracting...");
    std::thread([paneIndex, archivePath, entries = std::move(entries), destination, target = g_app.window] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = std::make_unique<ArchiveExtractResult>(); result->pane = paneIndex;
        result->openPath = destination; result->openInFiles4Me = true;
        result->result = SUCCEEDED(initialized) ? ExtractZipEntries(archivePath, entries, destination) : initialized;
        ArchiveExtractResult* raw = result.release();
        if (!PostMessageW(target, WM_APP_ARCHIVE_EXTRACT_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}

void OpenFocused() {
    Pane& pane = g_app.panes[g_app.activePane];
    int index = ListView_GetNextItem(pane.list, -1, LVNI_FOCUSED);
    if (pane.mode == PaneMode::DriveOverview) {
        if (index >= 0 && index < static_cast<int>(pane.driveItems.size()) && pane.driveItems[index].available)
            Navigate(g_app.activePane, pane.driveItems[index].root);
        return;
    }
    if (pane.mode == PaneMode::Archive) {
        if (index < 0 || index >= static_cast<int>(pane.items.size())) return;
        const FileItem& item = pane.items[index];
        if (item.IsDirectory()) Navigate(g_app.activePane, ArchiveLocation(pane.archivePath, item.archiveInternalPath));
        else {
            const std::wstring cache = ArchiveCacheFolder(); if (cache.empty()) return;
            ExtractArchiveAsync(g_app.activePane, {item.archiveInternalPath}, cache,
                                ArchiveExtractedPath(cache, item.archiveInternalPath));
        }
        return;
    }
    if (pane.mode != PaneMode::Filesystem || index < 0 || index >= static_cast<int>(pane.items.size())) return;
    const FileItem& item = pane.items[index];
    if (item.IsDirectory()) {
        Navigate(g_app.activePane, item.fullPath);
        return;
    }
    if (_wcsicmp(PathFindExtensionW(item.fullPath.c_str()), L".zip") == 0) {
        Navigate(g_app.activePane, ArchiveLocation(item.fullPath)); return;
    }
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.hwnd = g_app.window;
    info.lpVerb = L"open";
    info.lpFile = item.fullPath.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Open");
}

void AddTab(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    std::wstring path = pane.tabs.empty() ? DefaultPath() : pane.tabs[pane.activeTab].path;
    pane.tabs.push_back({path});
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    std::wstring text = DisplayNameForPath(path);
    item.pszText = text.data();
    pane.activeTab = TabCtrl_InsertItem(pane.tab, static_cast<int>(pane.tabs.size() - 1), &item);
    TabCtrl_SetCurSel(pane.tab, pane.activeTab);
    Navigate(paneIndex, path, false);
}

void CloseTab(int paneIndex) {
    Pane& pane = g_app.panes[paneIndex];
    if (pane.tabs.size() <= 1) return;
    pane.tabs.erase(pane.tabs.begin() + pane.activeTab);
    TabCtrl_DeleteItem(pane.tab, pane.activeTab);
    pane.activeTab = (std::min)(pane.activeTab, static_cast<int>(pane.tabs.size()) - 1);
    TabCtrl_SetCurSel(pane.tab, pane.activeTab);
    Navigate(paneIndex, pane.tabs[pane.activeTab].path, false);
}

std::array<int, 4> VisibleColumnWidths(const Pane& pane) {
    RECT client{};
    GetClientRect(pane.list, &client);
    int remaining = (std::max)(0, static_cast<int>(client.right - client.left));
    std::array<int, 4> visible{};
    for (int index = 0; index < 4; ++index) {
        visible[index] = (std::min)(pane.columnWidths[index], remaining);
        remaining -= visible[index];
    }
    return visible;
}

void UpdateListColumns(Pane& pane) {
    const auto visible = VisibleColumnWidths(pane);
    std::array<int, 4> current{};
    for (int index = 0; index < 4; ++index) current[index] = ListView_GetColumnWidth(pane.list, index);
    SendMessageW(pane.list, WM_SETREDRAW, FALSE, 0);
    // Apply shrinking columns before growing columns so the total width never
    // temporarily exceeds the viewport and creates a scrollbar flash.
    for (int index = 3; index >= 0; --index)
        if (visible[index] < current[index]) ListView_SetColumnWidth(pane.list, index, visible[index]);
    for (int index = 0; index < 4; ++index)
        if (visible[index] > current[index]) ListView_SetColumnWidth(pane.list, index, visible[index]);
    SendMessageW(pane.list, WM_SETREDRAW, TRUE, 0);
    ShowScrollBar(pane.list, SB_HORZ, FALSE);
}

void EnsureColumnWidths(Pane& pane, int width) {
    const bool recycle = pane.mode == PaneMode::RecycleBin;
    const int minimum = Scale(54);
    if (pane.columnLayoutWidth <= 0 || pane.columnLayoutMode != pane.mode) {
        if (recycle) {
            pane.columnWidths = {width * 28 / 100, width * 35 / 100, width * 23 / 100, width - width * 86 / 100};
        } else {
            pane.columnWidths = {width * 45 / 100, width * 14 / 100, width * 16 / 100, width - width * 75 / 100};
        }
        pane.columnLayoutMode = pane.mode;
    } else if (width != pane.columnLayoutWidth) {
        pane.columnWidths[3] = (std::max)(minimum, pane.columnWidths[3] + width - pane.columnLayoutWidth);
    }
    pane.columnLayoutWidth = width;
}

void ApplyLiveColumnResize(Pane& pane) {
    if (!pane.headers[0] || !pane.list) return;
    RECT first{};
    GetWindowRect(pane.headers[0], &first);
    MapWindowPoints(HWND_DESKTOP, g_app.window, reinterpret_cast<POINT*>(&first), 2);
    RECT oldLast{};
    GetWindowRect(pane.headers[3], &oldLast);
    MapWindowPoints(HWND_DESKTOP, g_app.window, reinterpret_cast<POINT*>(&oldLast), 2);
    RECT listRect{};
    GetWindowRect(pane.list, &listRect);
    MapWindowPoints(HWND_DESKTOP, g_app.window, reinterpret_cast<POINT*>(&listRect), 2);
    const int height = first.bottom - first.top;
    int x = first.left;

    // Treat the headers as one continuous strip.  Moving a divider changes only
    // the column on its left; every later column keeps its width and follows the
    // divider, matching Explorer's report view.
    HDWP positions = BeginDeferWindowPos(4);
    for (int index = 0; index < 4; ++index) {
        const int visibleX = (std::min)(x, static_cast<int>(listRect.right));
        const int visibleRight = (std::min)(x + pane.columnWidths[index], static_cast<int>(listRect.right));
        const int visibleWidth = (std::max)(0, visibleRight - visibleX);
        positions = DeferWindowPos(positions, pane.headers[index], nullptr, visibleX, first.top,
                                   visibleWidth, height,
                                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
        x += pane.columnWidths[index];
    }
    if (positions) EndDeferWindowPos(positions);

    // LVM_SETCOLUMNWIDTH can synchronously repaint several columns.  Suppress
    // that intermediate paint and present one completed frame instead.
    UpdateListColumns(pane);
    RECT headerStrip{first.left, first.top,
                     (std::min)((std::max)(static_cast<LONG>(x), oldLast.right), listRect.right), first.bottom};
    RedrawWindow(g_app.window, &headerStrip, nullptr,
                 RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RedrawWindow(pane.list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
}

LRESULT CALLBACK HeaderSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR paneAndColumn) {
    const int paneIndex = static_cast<int>(paneAndColumn / 4);
    const int column = static_cast<int>(paneAndColumn % 4);
    if (paneIndex < 0 || paneIndex > 1 || column < 0 || column > 3)
        return DefSubclassProc(window, message, wParam, lParam);
    Pane& pane = g_app.panes[paneIndex];
    auto nearDivider = [&]() {
        RECT client{}; GetClientRect(window, &client);
        POINT point{}; GetCursorPos(&point); ScreenToClient(window, &point);
        return column < 3 && point.x >= client.right - Scale(7);
    };
    switch (message) {
    case WM_SETCURSOR:
        if (pane.headerResizing || nearDivider()) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (nearDivider()) {
            POINT point{}; GetCursorPos(&point);
            pane.headerResizing = true;
            pane.resizeColumn = column;
            pane.resizeStartX = point.x;
            pane.resizeStartLeft = pane.columnWidths[column];
            SetCapture(window);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (pane.headerResizing && pane.resizeColumn == column && GetCapture() == window) {
            POINT point{}; GetCursorPos(&point);
            const int minimum = Scale(54);
            int delta = point.x - pane.resizeStartX;
            delta = (std::max)(minimum - pane.resizeStartLeft, delta);
            pane.columnWidths[column] = pane.resizeStartLeft + delta;
            ApplyLiveColumnResize(pane);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (pane.headerResizing && pane.resizeColumn == column) {
            pane.headerResizing = false;
            pane.resizeColumn = -1;
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED: case WM_CANCELMODE:
        pane.headerResizing = false;
        pane.resizeColumn = -1;
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, HeaderSubclassProc, subclassId);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool IsViewCommand(UINT id) { return id >= ID_VIEW_EXTRA_LARGE && id <= ID_VIEW_CONTENT; }

FileViewMode ViewModeFromCommand(UINT id) {
    return static_cast<FileViewMode>(id - ID_VIEW_EXTRA_LARGE);
}

UINT CommandFromViewMode(FileViewMode mode) {
    return ID_VIEW_EXTRA_LARGE + static_cast<UINT>(mode);
}

const wchar_t* ViewModeName(FileViewMode mode) {
    static const wchar_t* names[] = {L"Extra large icons", L"Large icons", L"Medium icons", L"Small icons",
                                     L"List", L"Details", L"Tiles", L"Content"};
    return names[static_cast<size_t>(mode)];
}

FileViewMode ParseViewMode(const std::wstring& value) {
    wchar_t* end = nullptr;
    const long parsed = wcstol(value.c_str(), &end, 10);
    return end && end != value.c_str() && *end == L'\0' && parsed >= 0 && parsed <= 7
        ? static_cast<FileViewMode>(parsed) : FileViewMode::Details;
}

int AddFolderImage(HIMAGELIST images, int pixels) {
    if (!images || pixels <= 0) return -1;
    HDC memory = CreateCompatibleDC(nullptr);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = pixels;
    bitmapInfo.bmiHeader.biHeight = -pixels;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    DWORD* bitmapPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &bitmapInfo, DIB_RGB_COLORS,
                                      reinterpret_cast<void**>(&bitmapPixels), nullptr, 0);
    if (!bitmap || !bitmapPixels) { DeleteDC(memory); return -1; }
    constexpr DWORD transparentPixel = 0x00FF00FF;
    std::fill_n(bitmapPixels, static_cast<size_t>(pixels) * pixels, transparentPixel);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    const int left = (std::max)(1, pixels * 7 / 100);
    const int right = pixels - left;
    const int tabTop = pixels * 19 / 100;
    const int bodyTop = pixels * 34 / 100;
    const int bottom = pixels * 84 / 100;
    const int tabRight = pixels * 51 / 100;
    HBRUSH folderBrush = CreateSolidBrush(RGB(255, 190, 38));
    HGDIOBJ oldPen = SelectObject(memory, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(memory, folderBrush);
    POINT tab[]{{left + pixels / 12, bodyTop + 1}, {left + pixels / 12, tabTop},
                {tabRight - pixels / 10, tabTop}, {tabRight, bodyTop + 1}};
    Polygon(memory, tab, ARRAYSIZE(tab));
    RoundRect(memory, left, bodyTop, right, bottom, (std::max)(2, pixels / 7), (std::max)(2, pixels / 7));
    SelectObject(memory, oldBrush); SelectObject(memory, oldPen);
    DeleteObject(folderBrush);
    SelectObject(memory, oldBitmap);
    for (size_t pixel = 0, count = static_cast<size_t>(pixels) * pixels; pixel < count; ++pixel) {
        bitmapPixels[pixel] = (bitmapPixels[pixel] & 0x00FFFFFF) == transparentPixel
            ? 0 : bitmapPixels[pixel] | 0xFF000000;
    }

    const int maskStride = ((pixels + 15) / 16) * 2;
    std::vector<BYTE> opaqueMask(static_cast<size_t>(maskStride) * pixels, 0);
    HBITMAP maskBitmap = CreateBitmap(pixels, pixels, 1, 1, opaqueMask.data());
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    const int index = icon ? ImageList_AddIcon(images, icon) : -1;
    if (icon) DestroyIcon(icon);
    if (maskBitmap) DeleteObject(maskBitmap);
    DeleteObject(bitmap); DeleteDC(memory);
    return index;
}

void RebuildIconCaches() {
    const int sourceSizes[] = {SHIL_JUMBO, SHIL_JUMBO, SHIL_EXTRALARGE};
    for (size_t index = 0; index < g_app.iconSources.size(); ++index) {
        g_app.iconSources[index].Reset();
        SHGetImageList(sourceSizes[index], IID_IImageList,
                       reinterpret_cast<void**>(g_app.iconSources[index].GetAddressOf()));
    }
    const int logicalSizes[] = {128, 72, 48};
    for (size_t index = 0; index < g_app.iconCaches.size(); ++index) {
        IconCache& cache = g_app.iconCaches[index];
        if (cache.images) ImageList_Destroy(cache.images);
        cache.pixels = Scale(logicalSizes[index]);
        cache.images = ImageList_Create(cache.pixels, cache.pixels, ILC_COLOR32 | ILC_MASK, 16, 16);
        if (cache.images) ImageList_SetBkColor(cache.images, CLR_NONE);
        cache.folderIndex = AddFolderImage(cache.images, cache.pixels);
        cache.indexes.clear();
    }
    IconCache& smallCache = g_app.smallIconCache;
    if (smallCache.images) ImageList_Destroy(smallCache.images);
    smallCache.pixels = (std::max)(Scale(26), GetSystemMetricsForDpi(SM_CXSMICON, g_app.dpi) + Scale(6));
    smallCache.images = ImageList_Create(smallCache.pixels, smallCache.pixels, ILC_COLOR32 | ILC_MASK, 32, 32);
    if (smallCache.images) ImageList_SetBkColor(smallCache.images, CLR_NONE);
    smallCache.folderIndex = AddFolderImage(smallCache.images, smallCache.pixels);
    smallCache.indexes.clear();
}

void RebuildSidebarIconCache() {
    if (g_app.sidebarImages) ImageList_Destroy(g_app.sidebarImages);
    const int iconPixels = GetSystemMetricsForDpi(SM_CXSMICON, g_app.dpi);
    const int rowPixels = (std::max)(Scale(24), iconPixels + Scale(4));
    g_app.sidebarImages = ImageList_Create(rowPixels, rowPixels, ILC_COLOR32 | ILC_MASK, 24, 16);
    if (g_app.sidebarImages) ImageList_SetBkColor(g_app.sidebarImages, CLR_NONE);
    g_app.sidebarIconIndexes.clear();
    if (g_app.sidebar) {
        ListView_SetImageList(g_app.sidebar, g_app.sidebarImages, LVSIL_SMALL);
        RebuildSidebar();
    }
}

int CachedSidebarIconIndex(int systemIndex) {
    if (systemIndex < 0) return -1;
    for (const auto& entry : g_app.sidebarIconIndexes)
        if (entry.first == systemIndex) return entry.second;
    if (!g_app.sidebarImages || !g_app.systemImages) return systemIndex;
    HICON icon = ImageList_GetIcon(g_app.systemImages, systemIndex, ILD_TRANSPARENT);
    if (!icon) return -1;
    int cellWidth = 0, cellHeight = 0;
    ImageList_GetIconSize(g_app.sidebarImages, &cellWidth, &cellHeight);
    const int iconPixels = (std::min)(Scale(20), (std::min)(cellWidth, cellHeight));
    HDC memory = CreateCompatibleDC(nullptr);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = cellWidth;
    bitmapInfo.bmiHeader.biHeight = -cellHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &bitmapInfo, DIB_RGB_COLORS,
                                      reinterpret_cast<void**>(&pixels), nullptr, 0);
    int added = -1;
    if (bitmap && pixels) {
        std::fill_n(pixels, static_cast<size_t>(cellWidth) * cellHeight, 0);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        DrawIconEx(memory, (cellWidth - iconPixels) / 2, (cellHeight - iconPixels) / 2,
                   icon, iconPixels, iconPixels, 0, nullptr, DI_NORMAL);
        SelectObject(memory, oldBitmap);
        for (size_t pixel = 0, count = static_cast<size_t>(cellWidth) * cellHeight; pixel < count; ++pixel) {
            if ((pixels[pixel] & 0xFF000000) == 0 && (pixels[pixel] & 0x00FFFFFF) != 0)
                pixels[pixel] |= 0xFF000000;
        }
        const int maskStride = ((cellWidth + 15) / 16) * 2;
        std::vector<BYTE> mask(static_cast<size_t>(maskStride) * cellHeight, 0);
        HBITMAP maskBitmap = CreateBitmap(cellWidth, cellHeight, 1, 1, mask.data());
        ICONINFO iconInfo{};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = bitmap;
        iconInfo.hbmMask = maskBitmap;
        HICON paddedIcon = CreateIconIndirect(&iconInfo);
        if (paddedIcon) {
            added = ImageList_AddIcon(g_app.sidebarImages, paddedIcon);
            DestroyIcon(paddedIcon);
        }
        if (maskBitmap) DeleteObject(maskBitmap);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    DestroyIcon(icon);
    if (added >= 0) g_app.sidebarIconIndexes.emplace_back(systemIndex, added);
    return added;
}

void RebuildCheckboxImages() {
    const int pixels = (std::max)(Scale(18), 16);
    HIMAGELIST images = ImageList_Create(pixels, pixels, ILC_COLOR32 | ILC_MASK, 3, 0);
    if (!images) return;
    ImageList_SetBkColor(images, CLR_NONE);

    const int maskStride = ((pixels + 15) / 16) * 2;
    const int colorStride = pixels * 4;
    std::vector<BYTE> andMask(static_cast<size_t>(maskStride) * pixels, 0xFF);
    std::vector<BYTE> xorBits(static_cast<size_t>(colorStride) * pixels, 0);
    HBITMAP transparentMask = CreateBitmap(pixels, pixels, 1, 1, andMask.data());
    HBITMAP transparentColor = CreateBitmap(pixels, pixels, 1, 32, xorBits.data());
    ICONINFO transparentInfo{};
    transparentInfo.fIcon = TRUE;
    transparentInfo.hbmMask = transparentMask;
    transparentInfo.hbmColor = transparentColor;
    HICON transparentIcon = CreateIconIndirect(&transparentInfo);
    if (transparentIcon) {
        ImageList_AddIcon(images, transparentIcon);
        DestroyIcon(transparentIcon);
    }
    if (transparentMask) DeleteObject(transparentMask);
    if (transparentColor) DeleteObject(transparentColor);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    constexpr COLORREF maskColor = RGB(255, 0, 255);
    for (int image = 1; image < 3; ++image) {
        HBITMAP bitmap = CreateCompatibleBitmap(screen, pixels, pixels);
        if (!bitmap) continue;
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        RECT bounds{0, 0, pixels, pixels};
        HBRUSH maskBrush = CreateSolidBrush(maskColor);
        FillRect(memory, &bounds, maskBrush);
        DeleteObject(maskBrush);

        const int inset = (std::max)(2, Scale(2));
        const int radius = (std::max)(4, Scale(5));
        const COLORREF boxColor = image == 2 ? g_app.colors.selection : g_app.colors.muted;
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, Scale(image == 2 ? 1 : 2)), boxColor);
        HBRUSH brush = CreateSolidBrush(image == 2 ? boxColor : g_app.colors.surface);
        HGDIOBJ oldPen = SelectObject(memory, pen);
        HGDIOBJ oldBrush = SelectObject(memory, brush);
        RoundRect(memory, inset, inset, pixels - inset, pixels - inset, radius, radius);
        SelectObject(memory, oldBrush);
        SelectObject(memory, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        if (image == 2) {
            HPEN checkPen = CreatePen(PS_SOLID, (std::max)(2, Scale(2)), g_app.colors.selectionText);
            oldPen = SelectObject(memory, checkPen);
            MoveToEx(memory, pixels * 27 / 100, pixels * 52 / 100, nullptr);
            LineTo(memory, pixels * 44 / 100, pixels * 69 / 100);
            LineTo(memory, pixels * 75 / 100, pixels * 34 / 100);
            SelectObject(memory, oldPen);
            DeleteObject(checkPen);
        }
        SelectObject(memory, oldBitmap);
        ImageList_AddMasked(images, bitmap, maskColor);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    HIMAGELIST previous = g_app.checkboxImages;
    g_app.checkboxImages = images;
    if (previous) ImageList_Destroy(previous);
}

int CachedIconIndex(int systemIndex, size_t cacheIndex, bool directory = false) {
    if (cacheIndex >= g_app.iconCaches.size()) return systemIndex;
    IconCache& cache = g_app.iconCaches[cacheIndex];
    if (directory && cache.folderIndex >= 0) return cache.folderIndex;
    for (const auto& entry : cache.indexes) if (entry.first == systemIndex) return entry.second;
    if (!cache.images) return systemIndex;
    HICON icon = nullptr;
    if (g_app.iconSources[cacheIndex])
        g_app.iconSources[cacheIndex]->GetIcon(systemIndex, ILD_TRANSPARENT, &icon);
    if (!icon && g_app.systemImagesLarge)
        icon = ImageList_GetIcon(g_app.systemImagesLarge, systemIndex, ILD_TRANSPARENT);
    if (!icon) return 0;
    const int added = ImageList_AddIcon(cache.images, icon);
    DestroyIcon(icon);
    if (added < 0) return 0;
    cache.indexes.emplace_back(systemIndex, added);
    return added;
}

int CachedSmallIconIndex(int systemIndex, bool directory = false) {
    IconCache& cache = g_app.smallIconCache;
    if (directory && cache.folderIndex >= 0) return cache.folderIndex;
    for (const auto& entry : cache.indexes) if (entry.first == systemIndex) return entry.second;
    if (!cache.images || !g_app.systemImages) return systemIndex;
    HICON icon = ImageList_GetIcon(g_app.systemImages, systemIndex, ILD_TRANSPARENT);
    if (!icon) return cache.folderIndex >= 0 ? cache.folderIndex : 0;
    const int added = ImageList_AddIcon(cache.images, icon);
    DestroyIcon(icon);
    if (added < 0) return cache.folderIndex >= 0 ? cache.folderIndex : 0;
    cache.indexes.emplace_back(systemIndex, added);
    return added;
}

void ConfigureTileView(Pane& pane) {
    if (pane.mode != PaneMode::DriveOverview && pane.view != FileViewMode::Tiles) return;
    LVTILEVIEWINFO info{sizeof(info)};
    info.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    info.dwFlags = LVTVIF_FIXEDSIZE;
    info.cLines = 1;
    info.sizeTile.cx = pane.mode == PaneMode::DriveOverview ? Scale(286) : Scale(220);
    info.sizeTile.cy = pane.mode == PaneMode::DriveOverview ? Scale(72) : Scale(56);
    ListView_SetTileViewInfo(pane.list, &info);
}

void ApplyPaneView(int paneIndex, FileViewMode mode) {
    if (paneIndex < 0 || paneIndex > 1) return;
    Pane& pane = g_app.panes[paneIndex];
    if (pane.mode != PaneMode::Filesystem) return;
    pane.view = mode;
    int nativeView = LV_VIEW_DETAILS;
    switch (mode) {
    case FileViewMode::ExtraLargeIcons: case FileViewMode::LargeIcons: case FileViewMode::MediumIcons:
        nativeView = LV_VIEW_ICON; break;
    case FileViewMode::SmallIcons: nativeView = LV_VIEW_SMALLICON; break;
    case FileViewMode::List: nativeView = LV_VIEW_LIST; break;
    case FileViewMode::Details: case FileViewMode::Content: nativeView = LV_VIEW_DETAILS; break;
    case FileViewMode::Tiles: nativeView = LV_VIEW_TILE; break;
    }
    ListView_SetView(pane.list, nativeView);
    HIMAGELIST smallImages = mode == FileViewMode::Content ? g_app.iconCaches[1].images : g_app.smallIconCache.images;
    HIMAGELIST normalImages = (mode <= FileViewMode::MediumIcons)
        ? g_app.iconCaches[static_cast<size_t>(mode)].images
        : (mode == FileViewMode::Tiles ? g_app.iconCaches[2].images
                                      : (g_app.systemImagesLarge ? g_app.systemImagesLarge : g_app.systemImages));
    ListView_SetImageList(pane.list, smallImages, LVSIL_SMALL);
    ListView_SetImageList(pane.list, normalImages, LVSIL_NORMAL);
    if (nativeView == LV_VIEW_ICON) {
        const int spacing = mode == FileViewMode::ExtraLargeIcons ? 164 : mode == FileViewMode::LargeIcons ? 112 : 88;
        ListView_SetIconSpacing(pane.list, Scale(spacing), Scale(spacing));
    }
    ConfigureTileView(pane);
    ListView_SetItemCountEx(pane.list, static_cast<int>(pane.items.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    LayoutWindow();
    InvalidateRect(pane.list, nullptr, TRUE);
    SaveSettings();
}

void LayoutPane(int paneIndex, RECT bounds) {
    Pane& pane = g_app.panes[paneIndex];
    const int pad = Scale(6), gap = Scale(4), rowHeight = Scale(30), headerHeight = Scale(27);
    const int statusHeight = g_app.preferences.showStatus ? Scale(26) : 0;
    int x = bounds.left + pad, y = bounds.top + pad;
    int width = bounds.right - bounds.left - pad * 2;
    ShowWindow(pane.tab, SW_HIDE);
    const int driveWidth = Scale(74);
    const int searchWidth = (std::min)(Scale(230), (std::max)(Scale(130), width / 3));
    MoveWindow(pane.drives, x, y, driveWidth, rowHeight, TRUE);
    MoveWindow(pane.path, x + driveWidth + gap, y, width - driveWidth - searchWidth - gap * 2, rowHeight, TRUE);
    MoveWindow(pane.search, x + width - searchWidth, y, searchWidth, rowHeight, TRUE); y += rowHeight + gap;
    const bool details = pane.mode == PaneMode::RecycleBin || pane.mode == PaneMode::Archive ||
                         (pane.mode == PaneMode::Filesystem && pane.view == FileViewMode::Details);
    EnsureColumnWidths(pane, width);
    int headerX = x;
    const int headerClipRight = x + width;
    for (int i = 0; i < 4; ++i) {
        const int visibleX = (std::min)(headerX, headerClipRight);
        const int visibleRight = (std::min)(headerX + pane.columnWidths[i], headerClipRight);
        MoveWindow(pane.headers[i], visibleX, y, (std::max)(0, visibleRight - visibleX), headerHeight, TRUE);
        ShowWindow(pane.headers[i], details ? SW_SHOW : SW_HIDE);
        headerX += pane.columnWidths[i];
    }
    if (details) y += headerHeight;
    const int listHeight = (std::max)(Scale(50), static_cast<int>(bounds.bottom - pad - statusHeight - y));
    MoveWindow(pane.list, x, y, width, listHeight, TRUE);
    if (details) UpdateListColumns(pane);
    else if (pane.view == FileViewMode::Content) {
        ListView_SetColumnWidth(pane.list, 0, width);
        for (int column = 1; column < 4; ++column) ListView_SetColumnWidth(pane.list, column, 0);
    }
    ConfigureTileView(pane);
    ShowScrollBar(pane.list, SB_HORZ, FALSE);
    MoveWindow(pane.status, x, y + listHeight, width, statusHeight, TRUE);
    ShowWindow(pane.status, g_app.preferences.showStatus ? SW_SHOW : SW_HIDE);
}

std::vector<std::shared_ptr<OperationJob>> JobSnapshot() {
    std::lock_guard lock(g_app.operations.mutex); return g_app.operations.jobs;
}

const wchar_t* JobKindText(JobKind kind) {
    switch (kind) {
    case JobKind::Copy: return L"Copying"; case JobKind::Move: return L"Moving";
    case JobKind::Recycle: return L"Recycling"; case JobKind::DeletePermanent: return L"Deleting";
    case JobKind::Rename: return L"Renaming"; default: return L"Creating folder";
    }
}

const wchar_t* JobStateText(JobState state) {
    switch (state) {
    case JobState::Queued: return L"Queued"; case JobState::Running: return L"Running";
    case JobState::Paused: return L"Paused"; case JobState::Cancelling: return L"Cancelling";
    case JobState::Completed: return L"Completed"; case JobState::Failed: return L"Failed";
    default: return L"Interrupted";
    }
}

int OperationDrawerHeight() {
    const auto jobs = JobSnapshot();
    return jobs.empty() ? 0 : Scale(g_app.operations.expanded ? 178 : 62);
}

std::shared_ptr<OperationJob> PrimaryOperationJob() {
    const auto jobs = JobSnapshot();
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        const JobState state = (*it)->state;
        if (state == JobState::Running || state == JobState::Paused || state == JobState::Queued ||
            state == JobState::Failed || state == JobState::Interrupted || state == JobState::Cancelling) return *it;
    }
    return jobs.empty() ? nullptr : jobs.back();
}

void UpdateOperationButtons() {
    auto job = PrimaryOperationJob();
    const bool visible = job != nullptr;
    for (HWND button : g_app.operationButtons) if (button) ShowWindow(button, visible ? SW_SHOW : SW_HIDE);
    if (!job) return;
    const JobState state = job->state;
    const wchar_t* primary = state == JobState::Paused ? L"Resume" :
                             (state == JobState::Failed || state == JobState::Interrupted) ? L"Retry" : L"Pause";
    SetWindowTextW(g_app.operationButtons[0], primary);
    EnableWindow(g_app.operationButtons[0], state != JobState::Completed && state != JobState::Cancelling);
    EnableWindow(g_app.operationButtons[1], state == JobState::Running || state == JobState::Paused || state == JobState::Queued);
    ShowWindow(g_app.operationButtons[0], state == JobState::Completed ? SW_HIDE : SW_SHOW);
    ShowWindow(g_app.operationButtons[1], state == JobState::Completed ? SW_HIDE : SW_SHOW);
}

void LayoutWindow() {
    RECT client{};
    GetClientRect(g_app.window, &client);
    const int toolbarInset = Scale(10), contentInset = Scale(12), top = Scale(76), gap = Scale(6);
    int x = toolbarInset;
    const int menuWidths[] = {44, 44, 48, 48};
    for (size_t i = 0; i < g_app.menuButtons.size(); ++i) {
        const int width = Scale(menuWidths[i]);
        MoveWindow(g_app.menuButtons[i], x, Scale(3), width, Scale(25), TRUE);
        x += width;
    }
    x = toolbarInset;
    const int toolY = Scale(34), toolHeight = Scale(34), toolGap = Scale(5);
    for (int i = 0; i < 4; ++i) {
        MoveWindow(g_app.commandButtons[i], x, toolY, Scale(36), toolHeight, TRUE);
        x += Scale(36) + toolGap;
    }
    x += Scale(8);
    MoveWindow(g_app.commandButtons[4], x, toolY, Scale(112), toolHeight, TRUE);
    x += Scale(112) + Scale(14);
    int right = client.right - toolbarInset;
    MoveWindow(g_app.commandButtons[7], right - Scale(124), toolY, Scale(124), toolHeight, TRUE); right -= Scale(124) + toolGap;
    MoveWindow(g_app.commandButtons[6], right - Scale(36), toolY, Scale(36), toolHeight, TRUE); right -= Scale(36) + toolGap;
    MoveWindow(g_app.commandButtons[5], right - Scale(36), toolY, Scale(36), toolHeight, TRUE);
    const int selectionRight = right - Scale(36) - toolGap;
    std::array<bool, 7> showSelection{};
    if (g_app.selectionActionsVisible) {
        if (g_app.panes[g_app.activePane].mode == PaneMode::RecycleBin)
            showSelection = {true, false, false, false, true, true, false};
        else if (g_app.panes[g_app.activePane].mode == PaneMode::Archive)
            showSelection = {false, true, false, false, false, true, true};
        else {
            showSelection = {true, true, true, true, true, true, false};
            showSelection[6] = !SelectedZipPath(g_app.activePane).empty();
        }
    }
    int visibleCount = static_cast<int>(std::count(showSelection.begin(), showSelection.end(), true));
    const int hidePriority[] = {3, 4, 0, 2, 1, 6};
    for (int index : hidePriority) {
        if (!visibleCount) break;
        if (x + visibleCount * Scale(36) + (visibleCount - 1) * toolGap <= selectionRight) break;
        showSelection[index] = false; --visibleCount;
    }
    for (size_t i = 0; i < g_app.selectionButtons.size(); ++i) {
        ShowWindow(g_app.selectionButtons[i], showSelection[i] ? SW_SHOW : SW_HIDE);
        if (!showSelection[i]) continue;
        MoveWindow(g_app.selectionButtons[i], x, toolY, Scale(36), toolHeight, TRUE);
        x += Scale(36) + toolGap;
    }
    const int drawerHeight = OperationDrawerHeight();
    RECT content{contentInset, top, client.right - contentInset,
                 client.bottom - Scale(30) - drawerHeight - (drawerHeight ? gap : 0)};
    if (g_app.sidebar && g_app.preferences.sidebarVisible) {
        const int available = static_cast<int>(content.right - content.left);
        const int sidebarWidth = (std::min)(Scale(216), (std::max)(Scale(160), available / 4));
        const int sidebarTopInset = Scale(10);
        MoveWindow(g_app.sidebar, content.left, content.top + sidebarTopInset, sidebarWidth,
                   content.bottom - content.top - sidebarTopInset, TRUE);
        // Keep the report column edge outside the visible client area. DarkMode_Explorer
        // otherwise paints its right boundary as a segmented vertical rule beside each row.
        ListView_SetColumnWidth(g_app.sidebar, 0, sidebarWidth + Scale(8));
        ShowScrollBar(g_app.sidebar, SB_HORZ, FALSE);
        ShowWindow(g_app.sidebar, SW_SHOW);
        content.left += sidebarWidth;
    } else if (g_app.sidebar) {
        ShowWindow(g_app.sidebar, SW_HIDE);
    }
    if (!g_app.dualPane) {
        LayoutPane(g_app.activePane, content);
        Pane& visible = g_app.panes[g_app.activePane];
        ShowWindow(visible.tab, SW_HIDE); ShowWindow(visible.drives, SW_SHOW); ShowWindow(visible.path, SW_SHOW); ShowWindow(visible.search, SW_SHOW);
        const bool details = visible.mode == PaneMode::RecycleBin ||
                             (visible.mode == PaneMode::Filesystem && visible.view == FileViewMode::Details);
        for (HWND header : visible.headers) ShowWindow(header, details ? SW_SHOW : SW_HIDE);
        ShowWindow(visible.list, SW_SHOW); ShowWindow(visible.status, SW_SHOW);
        for (int i = 0; i < 2; ++i) if (i != g_app.activePane) {
            Pane& pane = g_app.panes[i];
            ShowWindow(pane.tab, SW_HIDE); ShowWindow(pane.drives, SW_HIDE); ShowWindow(pane.path, SW_HIDE); ShowWindow(pane.search, SW_HIDE);
            for (HWND header : pane.headers) ShowWindow(header, SW_HIDE);
            ShowWindow(pane.list, SW_HIDE); ShowWindow(pane.status, SW_HIDE);
        }
    } else {
        const int half = (content.right - content.left - gap) / 2;
        LayoutPane(0, {content.left, content.top, content.left + half, content.bottom});
        LayoutPane(1, {content.left + half + gap, content.top, content.right, content.bottom});
        for (Pane& pane : g_app.panes) {
            ShowWindow(pane.tab, SW_HIDE); ShowWindow(pane.drives, SW_SHOW); ShowWindow(pane.path, SW_SHOW); ShowWindow(pane.search, SW_SHOW);
            const bool details = pane.mode == PaneMode::RecycleBin ||
                                 (pane.mode == PaneMode::Filesystem && pane.view == FileViewMode::Details);
            for (HWND header : pane.headers) ShowWindow(header, details ? SW_SHOW : SW_HIDE);
            ShowWindow(pane.list, SW_SHOW); ShowWindow(pane.status, SW_SHOW);
        }
    }
    if (drawerHeight) {
        const int drawerTop = client.bottom - Scale(30) - drawerHeight;
        int buttonRight = client.right - contentInset - Scale(10);
        const int buttonY = drawerTop + Scale(13);
        const int widths[] = {78, 76, 70};
        for (int i = 2; i >= 0; --i) {
            const int width = Scale(widths[i]);
            MoveWindow(g_app.operationButtons[i], buttonRight - width, buttonY, width, Scale(34), TRUE);
            buttonRight -= width + Scale(6);
        }
    }
    UpdateOperationButtons();
    InvalidateRect(g_app.window, nullptr, TRUE);
}

void ApplyTheme() {
    g_app.colors = GetColors(g_app.theme);
    ReleaseBrush(g_app.windowBrush); ReleaseBrush(g_app.surfaceBrush); ReleaseBrush(g_app.editBrush);
    g_app.windowBrush = CreateSolidBrush(g_app.colors.window);
    g_app.surfaceBrush = CreateSolidBrush(g_app.colors.surface);
    g_app.editBrush = CreateSolidBrush(g_app.colors.edit);
    RebuildCheckboxImages();
    SetClassLongPtrW(g_app.window, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g_app.windowBrush));
    const BOOL dark = g_app.theme == ThemeMode::Dark && !IsHighContrast();
    // Popup menus do not inherit DWMWA_USE_IMMERSIVE_DARK_MODE. On supported
    // Windows 10/11 builds, ask the system menu renderer to follow this app's
    // explicit theme. Resolve the private exports at runtime so older systems
    // retain their normal menus and Shell extension owner-draw data stays intact.
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    using FlushMenuThemesFn = void(WINAPI*)();
    static SetPreferredAppModeFn setPreferredAppMode = nullptr;
    static FlushMenuThemesFn flushMenuThemes = nullptr;
    static PreferredAppMode appliedMenuMode = PreferredAppMode::Max;
    static const bool menuThemeAvailable = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW version{sizeof(version)};
        if (!rtlGetVersion || rtlGetVersion(&version) != 0 || version.dwBuildNumber < 18362) return false;
        HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!theme) return false;
        setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
        flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(theme, MAKEINTRESOURCEA(136)));
        return setPreferredAppMode != nullptr;
    }();
    if (menuThemeAvailable) {
        const PreferredAppMode requestedMode = IsHighContrast() ? PreferredAppMode::Default
                                                                 : (dark ? PreferredAppMode::ForceDark
                                                                         : PreferredAppMode::ForceLight);
        if (requestedMode != appliedMenuMode) {
            setPreferredAppMode(requestedMode);
            appliedMenuMode = requestedMode;
            if (flushMenuThemes) flushMenuThemes();
        }
    }
    DwmSetWindowAttribute(g_app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const int roundedPreference = 2;
    DwmSetWindowAttribute(g_app.window, static_cast<DWMWINDOWATTRIBUTE>(33), &roundedPreference, sizeof(roundedPreference));
    if (g_app.sidebar) {
        SetWindowTheme(g_app.sidebar, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ListView_SetBkColor(g_app.sidebar, g_app.colors.window);
        ListView_SetTextBkColor(g_app.sidebar, g_app.colors.window);
        ListView_SetTextColor(g_app.sidebar, g_app.colors.text);
        InvalidateRect(g_app.sidebar, nullptr, TRUE);
    }
    for (Pane& pane : g_app.panes) {
        SetWindowTheme(pane.list, pane.mode == PaneMode::DriveOverview ? L"" : (dark ? L"DarkMode_Explorer" : L"Explorer"),
                       pane.mode == PaneMode::DriveOverview ? L"" : nullptr);
        SetWindowTheme(pane.path, dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
        SetWindowTheme(pane.search, dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
        ListView_SetBkColor(pane.list, g_app.colors.surface);
        ListView_SetTextBkColor(pane.list, g_app.colors.surface);
        ListView_SetTextColor(pane.list, g_app.colors.text);
        InvalidateRect(pane.tab, nullptr, TRUE); InvalidateRect(pane.path, nullptr, TRUE); InvalidateRect(pane.search, nullptr, TRUE);
        for (HWND header : pane.headers) InvalidateRect(header, nullptr, TRUE);
        InvalidateRect(pane.list, nullptr, TRUE); InvalidateRect(pane.status, nullptr, TRUE);
    }
    for (HWND button : g_app.commandButtons) InvalidateRect(button, nullptr, TRUE);
    for (HWND button : g_app.selectionButtons) InvalidateRect(button, nullptr, TRUE);
    for (HWND button : g_app.menuButtons) InvalidateRect(button, nullptr, TRUE);
    for (HWND button : g_app.operationButtons) if (button) InvalidateRect(button, nullptr, TRUE);
    if (g_app.layoutPopup) {
        DwmSetWindowAttribute(g_app.layoutPopup, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        for (HWND button : g_app.layoutButtons) if (button) InvalidateRect(button, nullptr, TRUE);
        InvalidateRect(g_app.layoutPopup, nullptr, TRUE);
    }
    if (g_app.settingsWindow) {
        DwmSetWindowAttribute(g_app.settingsWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(g_app.settingsWindow, static_cast<DWMWINDOWATTRIBUTE>(33), &roundedPreference, sizeof(roundedPreference));
        InvalidateRect(g_app.settingsWindow, nullptr, TRUE);
    }
    InvalidateRect(g_app.window, nullptr, TRUE);
}

bool IsMenuButton(UINT id) { return id >= ID_MENU_FILE && id <= ID_MENU_HELP; }
bool IsIconOnlyButton(UINT id) {
    return (id >= ID_BACK && id <= ID_REFRESH) || id == ID_THEME_TOGGLE || id == ID_CLIP_CUT ||
           id == ID_CLIP_COPY || id == ID_CLIP_PASTE || id == ID_RENAME || id == ID_DELETE || id == ID_MORE ||
           id == ID_VIEW_LAYOUT || id == ID_EXTRACT_ALL;
}
bool IsColumnHeader(UINT id) {
    if (id < ID_PANE_BASE) return false;
    const UINT local = (id - ID_PANE_BASE) % ID_PANE_STRIDE;
    return local >= ID_HEADER_NAME && local <= ID_HEADER_DATE;
}
bool HasCommandIcon(UINT id) {
    return IsIconOnlyButton(id) || IsViewCommand(id) || id == ID_NEW_FOLDER || id == ID_LIGHT || id == ID_DARK || id == ID_DUAL;
}

int NewTemplateIcon(UINT command) {
    if (command < ID_NEW_TEMPLATE_BASE || command > ID_NEW_TEMPLATE_LAST) return -1;
    const size_t index = command - ID_NEW_TEMPLATE_BASE;
    return index < g_app.newItemTemplates.size() ? g_app.newItemTemplates[index].icon : -1;
}

wchar_t MaterialGlyph(UINT id) {
    switch (id) {
    case ID_BACK: return L'\ue5c4';
    case ID_FORWARD: return L'\ue5c8';
    case ID_UP: return L'\ue5d8';
    case ID_REFRESH: return L'\ue5d5';
    case ID_NEW_FOLDER:
        return g_app.panes[g_app.activePane].mode == PaneMode::RecycleBin ? L'\ue872' :
               g_app.panes[g_app.activePane].mode == PaneMode::Archive ? L'\ue169' : L'\ue2cc';
    case ID_CREATE_FOLDER: return L'\ue2cc';
    case ID_CREATE_TEXT: return L'\ue873';
    case ID_LIGHT: return L'\ue518';
    case ID_DARK: return L'\ue51c';
    case ID_THEME_TOGGLE: return g_app.theme == ThemeMode::Light ? L'\ue518' : L'\ue51c';
    case ID_VIEW_LAYOUT: return L'\ue8f0';
    case ID_DUAL: return g_app.dualPane ? L'\ue8ec' : L'\ue069';
    case ID_CLIP_CUT: return g_app.panes[g_app.activePane].mode == PaneMode::RecycleBin ? L'\ue8b3' : L'\ue14e';
    case ID_CLIP_COPY: return L'\ue14d';
    case ID_CLIP_PASTE: return L'\ue14f';
    case ID_RENAME: return L'\ue9a2';
    case ID_DELETE: return L'\ue872';
    case ID_MORE: return L'\ue5d4';
    case ID_EXTRACT_ALL: return L'\ue169';
    case ID_VIEW_EXTRA_LARGE: return L'\ue3f4';
    case ID_VIEW_LARGE: return L'\ue3f4';
    case ID_VIEW_MEDIUM: return L'\ue3f4';
    case ID_VIEW_SMALL: return L'\ue3f4';
    case ID_VIEW_LIST: return L'\ue896';
    case ID_VIEW_DETAILS: return L'\ue8ef';
    case ID_VIEW_TILES: return L'\ue8f0';
    case ID_VIEW_CONTENT: return L'\ue8f0';
    case ID_OPEN: return L'\ue89e';
    case ID_OPEN_WITH: return L'\ue7ac';
    case ID_COPY: return L'\ue2c6';
    case ID_MOVE: return L'\ue2c8';
    case ID_COPY_PATH: return L'\ue157';
    case ID_PROPERTIES: return L'\ue88e';
    case ID_DELETE_PERMANENT: return L'\ue92b';
    case ID_SHELL_CONTEXT: return L'\ue5d3';
    case ID_NEW_TAB: return L'\ue89c';
    case ID_CLOSE_TAB: return L'\ue5cd';
    case ID_SETTINGS: return L'\ue8b8';
    case ID_EXIT: return L'\ue879';
    case ID_SELECT_ALL: return L'\ue834';
    case ID_ABOUT: return L'\ue88e';
    case ID_SHORTCUTS: return L'\ue312';
    case ID_CHECK_UPDATES: return L'\ue863';
    default: return 0;
    }
}

bool IsPreferenceOn(UINT id) {
    switch (id) {
    case ID_PREF_RESTORE: return g_app.preferences.restorePaths;
    case ID_PREF_CONFIRM_DELETE: return g_app.preferences.confirmPermanentDelete;
    case ID_PREF_STATUS: return g_app.preferences.showStatus;
    case ID_PREF_HIDDEN: return g_app.preferences.showHiddenSystem;
    case ID_PREF_DIRS_FIRST: return g_app.preferences.directoriesFirst;
    case ID_PREF_EXTENSIONS: return g_app.preferences.showExtensions;
    case ID_PREF_CONTEXT_MENU: return g_app.preferences.shellContextMenu;
    case ID_PREF_DRAG_MOVE: return g_app.preferences.dragMoveSameDrive;
    case ID_PREF_AUTO_REFRESH: return g_app.preferences.autoRefresh;
    case ID_PREF_REFRESH_SPEED: return g_app.preferences.refreshMilliseconds <= 1500;
    case ID_PREF_SIDEBAR_VISIBLE: return g_app.preferences.sidebarVisible;
    case ID_PREF_GALLERY: return g_app.preferences.sidebarGallery;
    case ID_PREF_RECYCLE: return g_app.preferences.sidebarRecycleBin;
    case ID_PREF_NETWORK: return g_app.preferences.sidebarNetwork;
    case ID_PREF_LINUX: return g_app.preferences.sidebarLinux;
    case ID_PREF_THEME_LIGHT: return g_app.theme == ThemeMode::Light;
    case ID_PREF_THEME_DARK: return g_app.theme == ThemeMode::Dark;
    case ID_PREF_DUAL: return g_app.dualPane;
    case ID_PREF_AUTO_UPDATES: return g_app.preferences.automaticUpdates;
    default: return false;
    }
}

void DrawCommandIcon(HDC dc, UINT id, RECT area, COLORREF color, COLORREF background) {
    const int stroke = (std::max)(1, Scale(2));
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int left = area.left, right = area.right, top = area.top, bottom = area.bottom;
    const int middleX = (left + right) / 2, middleY = (top + bottom) / 2;
    if (id == ID_BACK || id == ID_FORWARD) {
        const bool forward = id == ID_FORWARD;
        const int tip = forward ? right - Scale(4) : left + Scale(4);
        const int tail = forward ? left + Scale(5) : right - Scale(5);
        MoveToEx(dc, tail, middleY, nullptr); LineTo(dc, tip, middleY);
        MoveToEx(dc, tip, middleY, nullptr); LineTo(dc, forward ? middleX : middleX, top + Scale(4));
        MoveToEx(dc, tip, middleY, nullptr); LineTo(dc, middleX, bottom - Scale(4));
    } else if (id == ID_UP) {
        MoveToEx(dc, middleX, top + Scale(3), nullptr); LineTo(dc, left + Scale(4), middleY);
        MoveToEx(dc, middleX, top + Scale(3), nullptr); LineTo(dc, right - Scale(4), middleY);
        MoveToEx(dc, middleX, top + Scale(4), nullptr); LineTo(dc, middleX, bottom - Scale(3));
    } else if (id == ID_REFRESH) {
        POINT refresh[] = {{right - Scale(4), middleY + Scale(2)}, {right - Scale(5), top + Scale(6)},
                           {middleX, top + Scale(3)}, {left + Scale(5), top + Scale(6)},
                           {left + Scale(3), middleY}, {left + Scale(6), bottom - Scale(5)},
                           {middleX, bottom - Scale(3)}, {right - Scale(5), bottom - Scale(6)}};
        Polyline(dc, refresh, ARRAYSIZE(refresh));
        MoveToEx(dc, right - Scale(4), middleY + Scale(2), nullptr); LineTo(dc, right - Scale(4), top + Scale(5));
        MoveToEx(dc, right - Scale(4), middleY + Scale(2), nullptr); LineTo(dc, right - Scale(9), middleY + Scale(2));
    } else if (id == ID_NEW_FOLDER) {
        POINT outline[] = {{left + Scale(2), top + Scale(7)}, {left + Scale(8), top + Scale(7)},
                           {left + Scale(11), top + Scale(10)}, {right - Scale(2), top + Scale(10)},
                           {right - Scale(2), bottom - Scale(3)}, {left + Scale(2), bottom - Scale(3)},
                           {left + Scale(2), top + Scale(7)}};
        Polyline(dc, outline, ARRAYSIZE(outline));
        MoveToEx(dc, middleX + Scale(3), middleY + Scale(1), nullptr); LineTo(dc, right - Scale(5), middleY + Scale(1));
        MoveToEx(dc, middleX + Scale(6), middleY - Scale(2), nullptr); LineTo(dc, middleX + Scale(6), middleY + Scale(5));
    } else if (id == ID_LIGHT) {
        Ellipse(dc, middleX - Scale(4), middleY - Scale(4), middleX + Scale(4), middleY + Scale(4));
        for (int offsetX = -1; offsetX <= 1; ++offsetX) for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            if (offsetX == 0 && offsetY == 0) continue;
            MoveToEx(dc, middleX + offsetX * Scale(7), middleY + offsetY * Scale(7), nullptr);
            LineTo(dc, middleX + offsetX * Scale(10), middleY + offsetY * Scale(10));
        }
    } else if (id == ID_DARK) {
        HBRUSH moon = CreateSolidBrush(color);
        HGDIOBJ previousBrush = SelectObject(dc, moon);
        Ellipse(dc, left + Scale(4), top + Scale(3), right - Scale(4), bottom - Scale(3));
        HBRUSH cutout = CreateSolidBrush(background);
        SelectObject(dc, cutout);
        HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, middleX, top + Scale(1), right, bottom - Scale(6));
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
        DeleteObject(cutout); DeleteObject(moon);
    } else if (id == ID_DUAL) {
        Rectangle(dc, left + Scale(2), top + Scale(4), middleX - Scale(1), bottom - Scale(4));
        Rectangle(dc, middleX + Scale(2), top + Scale(4), right - Scale(2), bottom - Scale(4));
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawOwnerControl(const DRAWITEMSTRUCT* draw) {
    RECT rect = draw->rcItem;
    const bool menuButton = IsMenuButton(draw->CtlID);
    const bool toolbarButton = std::find(g_app.commandButtons.begin(), g_app.commandButtons.end(), draw->hwndItem) != g_app.commandButtons.end() ||
                               std::find(g_app.selectionButtons.begin(), g_app.selectionButtons.end(), draw->hwndItem) != g_app.selectionButtons.end();
    const bool columnHeader = IsColumnHeader(draw->CtlID);
    const bool settingsCategoryButton = draw->CtlID >= ID_SETTINGS_CATEGORY_BASE && draw->CtlID < ID_SETTINGS_CATEGORY_BASE + 8;
    const bool preferenceButton = (draw->CtlID >= ID_PREF_RESTORE && draw->CtlID <= ID_PREF_DUAL) || draw->CtlID == ID_PREF_AUTO_UPDATES;
    const bool selectedTheme = (((draw->CtlID >= ID_PREF_RESTORE && draw->CtlID <= ID_PREF_DUAL) || draw->CtlID == ID_PREF_AUTO_UPDATES) && IsPreferenceOn(draw->CtlID)) ||
                               (draw->CtlID >= ID_SETTINGS_CATEGORY_BASE && draw->CtlID < ID_SETTINGS_CATEGORY_BASE + 8 &&
                                static_cast<int>(draw->CtlID - ID_SETTINGS_CATEGORY_BASE) == g_app.settingsCategory) ||
                               (IsViewCommand(draw->CtlID) && CommandFromViewMode(g_app.panes[g_app.activePane].view) == draw->CtlID);
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool hot = (draw->itemState & ODS_HOTLIGHT) != 0 || g_app.hoveredToolbarButton == draw->hwndItem;
    COLORREF fill = columnHeader ? ((pressed || hot) ? g_app.colors.buttonPressed : g_app.colors.surface) :
                    settingsCategoryButton ? ((pressed || hot || selectedTheme) ? g_app.colors.buttonPressed : g_app.colors.window) :
                    preferenceButton ? ((pressed || hot) ? g_app.colors.buttonPressed : g_app.colors.button) :
                    (menuButton || toolbarButton) ? ((pressed || hot) ? g_app.colors.buttonPressed : ToolbarColor()) :
                    ((pressed || hot || selectedTheme) ? g_app.colors.buttonPressed : g_app.colors.button);
    HBRUSH brush = CreateSolidBrush(fill);
    if (menuButton || toolbarButton || IsColumnHeader(draw->CtlID)) {
        FillRect(draw->hDC, &rect, brush);
    } else {
        const bool surfaceBackground = draw->CtlID >= ID_PANE_BASE || preferenceButton || IsViewCommand(draw->CtlID);
        FillRect(draw->hDC, &rect, surfaceBackground ? g_app.surfaceBrush : g_app.windowBrush);
        if ((!toolbarButton || hot || pressed) && (!settingsCategoryButton || hot || pressed || selectedTheme)) {
            HRGN rounded = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, Scale(10), Scale(10));
            FillRgn(draw->hDC, rounded, brush);
            DeleteObject(rounded);
        }
    }
    DeleteObject(brush);

    wchar_t text[256]{};
    GetWindowTextW(draw->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(draw->hDC, TRANSPARENT);
    const COLORREF foreground = (draw->itemState & ODS_DISABLED) != 0 ? g_app.colors.muted : g_app.colors.text;
    SetTextColor(draw->hDC, foreground);
    SelectObject(draw->hDC, selectedTheme ? g_app.fontBold : g_app.font);
    RECT textRect = rect;
    if (HasCommandIcon(draw->CtlID)) {
        RECT iconRect{};
        if (IsIconOnlyButton(draw->CtlID)) {
            const int iconSize = Scale(22);
            iconRect = {(rect.left + rect.right - iconSize) / 2, (rect.top + rect.bottom - iconSize) / 2,
                        (rect.left + rect.right + iconSize) / 2, (rect.top + rect.bottom + iconSize) / 2};
        } else {
            SIZE textSize{};
            GetTextExtentPoint32W(draw->hDC, text, static_cast<int>(wcslen(text)), &textSize);
            const int iconSize = Scale(22), gap = Scale(7);
            const int available = static_cast<int>(rect.right - rect.left) - Scale(16);
            const int groupWidth = (std::min)(available, iconSize + gap + static_cast<int>(textSize.cx));
            const int groupLeft = rect.left + (static_cast<int>(rect.right - rect.left) - groupWidth) / 2;
            iconRect = {groupLeft, (rect.top + rect.bottom - iconSize) / 2,
                        groupLeft + iconSize, (rect.top + rect.bottom + iconSize) / 2};
            textRect.left = iconRect.right + gap;
            textRect.right = rect.right - Scale(8);
        }
        const wchar_t glyph = MaterialGlyph(draw->CtlID);
        if (g_app.iconFont && glyph) {
            const wchar_t textGlyph[] = {glyph, L'\0'};
            HGDIOBJ previousFont = SelectObject(draw->hDC, g_app.iconFont);
            DrawTextW(draw->hDC, textGlyph, 1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(draw->hDC, previousFont);
        } else {
            DrawCommandIcon(draw->hDC, draw->CtlID, iconRect, foreground, fill);
        }
    }
    if (!IsIconOnlyButton(draw->CtlID)) {
        if (menuButton) textRect.left += Scale(8);
        if (settingsCategoryButton || preferenceButton) textRect.left += Scale(12);
        if (preferenceButton) textRect.right -= Scale(58);
        DrawTextW(draw->hDC, text, -1, &textRect,
                  ((menuButton || HasCommandIcon(draw->CtlID) || settingsCategoryButton || preferenceButton) ? DT_LEFT : DT_CENTER) |
                  DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (settingsCategoryButton && selectedTheme) {
        RECT accent{rect.left + Scale(2), rect.top + Scale(7), rect.left + Scale(5), rect.bottom - Scale(7)};
        HBRUSH accentBrush = CreateSolidBrush(g_app.colors.selection);
        FillRect(draw->hDC, &accent, accentBrush); DeleteObject(accentBrush);
    }
    if (preferenceButton) {
        RECT track{rect.right - Scale(48), (rect.top + rect.bottom - Scale(20)) / 2,
                   rect.right - Scale(12), (rect.top + rect.bottom + Scale(20)) / 2};
        const COLORREF toggleOn = g_app.theme == ThemeMode::Dark ? g_app.colors.selection : RGB(0, 120, 215);
        const COLORREF toggleOff = g_app.theme == ThemeMode::Dark ? RGB(92, 92, 92) : RGB(170, 170, 170);
        HBRUSH trackBrush = CreateSolidBrush(selectedTheme ? toggleOn : toggleOff);
        HRGN trackRegion = CreateRoundRectRgn(track.left, track.top, track.right + 1, track.bottom + 1, Scale(20), Scale(20));
        FillRgn(draw->hDC, trackRegion, trackBrush); DeleteObject(trackRegion); DeleteObject(trackBrush);
        SetTextColor(draw->hDC, RGB(255, 255, 255)); SelectObject(draw->hDC, g_app.fontBold);
        DrawTextW(draw->hDC, selectedTheme ? L"On" : L"Off", -1, &track, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (draw->CtlID >= ID_PANE_BASE) {
        const int paneIndex = static_cast<int>((draw->CtlID - ID_PANE_BASE) / ID_PANE_STRIDE);
        const UINT localId = (draw->CtlID - ID_PANE_BASE) % ID_PANE_STRIDE;
        if (paneIndex >= 0 && paneIndex < 2 && localId >= ID_HEADER_NAME && localId <= ID_HEADER_DATE) {
            const SortColumn column = static_cast<SortColumn>(localId - ID_HEADER_NAME);
            const Pane& pane = g_app.panes[paneIndex];
            if (pane.sort == column) {
                const int x = rect.right - Scale(12), y = (rect.top + rect.bottom) / 2;
                POINT triangle[3]{};
                if (pane.ascending) triangle[0] = {x, y - Scale(3)}, triangle[1] = {x - Scale(4), y + Scale(3)}, triangle[2] = {x + Scale(4), y + Scale(3)};
                else triangle[0] = {x, y + Scale(3)}, triangle[1] = {x - Scale(4), y - Scale(3)}, triangle[2] = {x + Scale(4), y - Scale(3)};
                HBRUSH indicator = CreateSolidBrush(g_app.colors.text);
                HGDIOBJ previousBrush = SelectObject(draw->hDC, indicator);
                HGDIOBJ previousPen = SelectObject(draw->hDC, GetStockObject(NULL_PEN));
                Polygon(draw->hDC, triangle, ARRAYSIZE(triangle));
                SelectObject(draw->hDC, previousPen); SelectObject(draw->hDC, previousBrush);
                DeleteObject(indicator);
            }
        }
    }
    if (columnHeader) {
        HPEN divider = CreatePen(PS_SOLID, 1, g_app.colors.border);
        HGDIOBJ previousPen = SelectObject(draw->hDC, divider);
        MoveToEx(draw->hDC, rect.left, rect.bottom - 1, nullptr);
        LineTo(draw->hDC, rect.right, rect.bottom - 1);
        const int separatorX = (std::max)(static_cast<int>(rect.left), static_cast<int>(rect.right) - 2);
        MoveToEx(draw->hDC, separatorX, rect.top + Scale(4), nullptr);
        LineTo(draw->hDC, separatorX, rect.bottom);
        SelectObject(draw->hDC, previousPen);
        DeleteObject(divider);
    }
}

void PrepareThemedMenuRecursive(HMENU menu) {
    if (!menu) return;
    MENUINFO menuInfo{sizeof(menuInfo)};
    menuInfo.fMask = MIM_BACKGROUND;
    menuInfo.hbrBack = g_app.surfaceBrush;
    SetMenuInfo(menu, &menuInfo);
    const int count = GetMenuItemCount(menu);
    for (int position = 0; position < count; ++position) {
        wchar_t textBuffer[512]{};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_ID | MIIM_STRING;
        info.dwTypeData = textBuffer; info.cch = ARRAYSIZE(textBuffer) - 1;
        if (!GetMenuItemInfoW(menu, position, TRUE, &info)) continue;
        auto visual = std::make_unique<ThemedMenuItem>();
        visual->separator = (info.fType & MFT_SEPARATOR) != 0;
        visual->submenu = info.hSubMenu != nullptr;
        visual->command = info.wID;
        visual->text = textBuffer;
        ThemedMenuItem* data = visual.get();
        g_app.activeMenuItems.push_back(std::move(visual));
        MENUITEMINFOW ownerDraw{sizeof(ownerDraw)};
        ownerDraw.fMask = MIIM_FTYPE | MIIM_DATA;
        ownerDraw.fType = MFT_OWNERDRAW;
        ownerDraw.dwItemData = reinterpret_cast<ULONG_PTR>(data);
        SetMenuItemInfoW(menu, position, TRUE, &ownerDraw);
        if (info.hSubMenu) PrepareThemedMenuRecursive(info.hSubMenu);
    }
}

void PrepareThemedMenu(HMENU menu) {
    g_app.activeMenuItems.clear();
    PrepareThemedMenuRecursive(menu);
}

bool MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure) {
    if (!measure || measure->CtlType != ODT_MENU || !measure->itemData) return false;
    auto* item = reinterpret_cast<ThemedMenuItem*>(measure->itemData);
    if (item->magic != ThemedMenuItem::Magic) return false;
    if (item->separator) {
        measure->itemWidth = Scale(180); measure->itemHeight = Scale(9); return true;
    }
    HDC dc = GetDC(g_app.window); HGDIOBJ oldFont = SelectObject(dc, g_app.font);
    std::wstring primary = item->text, shortcut;
    const size_t tab = primary.find(L'\t');
    if (tab != std::wstring::npos) { shortcut = primary.substr(tab + 1); primary.resize(tab); }
    SIZE primarySize{}, shortcutSize{};
    GetTextExtentPoint32W(dc, primary.c_str(), static_cast<int>(primary.size()), &primarySize);
    if (!shortcut.empty()) GetTextExtentPoint32W(dc, shortcut.c_str(), static_cast<int>(shortcut.size()), &shortcutSize);
    SelectObject(dc, oldFont); ReleaseDC(g_app.window, dc);
    const int measuredWidth = static_cast<int>(primarySize.cx + shortcutSize.cx) + Scale(shortcut.empty() ? 70 : 105);
    measure->itemWidth = static_cast<UINT>((std::max)(Scale(240), (std::min)(Scale(380), measuredWidth)));
    measure->itemHeight = static_cast<UINT>(Scale(32));
    return true;
}

bool DrawThemedMenuItem(DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_MENU || !draw->itemData) return false;
    auto* item = reinterpret_cast<ThemedMenuItem*>(draw->itemData);
    if (item->magic != ThemedMenuItem::Magic) return false;
    FillRect(draw->hDC, &draw->rcItem, g_app.surfaceBrush);
    if (item->separator) {
        RECT line{draw->rcItem.left + Scale(12), (draw->rcItem.top + draw->rcItem.bottom) / 2,
                  draw->rcItem.right - Scale(12), (draw->rcItem.top + draw->rcItem.bottom) / 2 + 1};
        HBRUSH brush = CreateSolidBrush(g_app.colors.border); FillRect(draw->hDC, &line, brush); DeleteObject(brush);
        return true;
    }
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    if (selected) {
        RECT highlight = draw->rcItem; InflateRect(&highlight, -Scale(4), -Scale(2));
        HBRUSH brush = CreateSolidBrush(g_app.colors.buttonPressed);
        HRGN region = CreateRoundRectRgn(highlight.left, highlight.top, highlight.right + 1, highlight.bottom + 1, Scale(7), Scale(7));
        FillRgn(draw->hDC, region, brush); DeleteObject(region); DeleteObject(brush);
    }
    SetBkMode(draw->hDC, TRANSPARENT); SelectObject(draw->hDC, g_app.font);
    SetTextColor(draw->hDC, disabled ? g_app.colors.muted : g_app.colors.text);
    std::wstring primary = item->text, shortcut;
    const size_t tab = primary.find(L'\t');
    if (tab != std::wstring::npos) { shortcut = primary.substr(tab + 1); primary.resize(tab); }
    RECT textRect{draw->rcItem.left + Scale(34), draw->rcItem.top,
                  draw->rcItem.right - Scale(item->submenu ? 28 : 12), draw->rcItem.bottom};
    const int shellIcon = NewTemplateIcon(item->command);
    const wchar_t glyph = MaterialGlyph(item->command);
    if (shellIcon >= 0 && g_app.systemImages && (draw->itemState & ODS_CHECKED) == 0) {
        int width = 16, height = 16;
        ImageList_GetIconSize(g_app.systemImages, &width, &height);
        const int x = draw->rcItem.left + Scale(18) - width / 2;
        const int y = (draw->rcItem.top + draw->rcItem.bottom - height) / 2;
        ImageList_Draw(g_app.systemImages, shellIcon, draw->hDC, x, y,
                       ILD_TRANSPARENT | (disabled ? ILD_BLEND50 : 0));
    } else if (glyph && (draw->itemState & ODS_CHECKED) == 0) {
        RECT iconRect{draw->rcItem.left + Scale(8), draw->rcItem.top,
                      draw->rcItem.left + Scale(28), draw->rcItem.bottom};
        SetTextColor(draw->hDC, disabled ? g_app.colors.muted : g_app.colors.text);
        const wchar_t iconText[] = {glyph, L'\0'};
        HGDIOBJ previousFont = SelectObject(draw->hDC, g_app.iconFont ? g_app.iconFont : g_app.font);
        DrawTextW(draw->hDC, iconText, 1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(draw->hDC, previousFont);
    }
    DrawTextW(draw->hDC, primary.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!shortcut.empty()) {
        RECT shortcutRect{textRect.left, textRect.top, draw->rcItem.right - Scale(14), textRect.bottom};
        SetTextColor(draw->hDC, g_app.colors.muted);
        DrawTextW(draw->hDC, shortcut.c_str(), -1, &shortcutRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if ((draw->itemState & ODS_CHECKED) != 0) {
        RECT checkRect{draw->rcItem.left + Scale(8), draw->rcItem.top,
                       draw->rcItem.left + Scale(28), draw->rcItem.bottom};
        SetTextColor(draw->hDC, g_app.colors.text);
        DrawTextW(draw->hDC, L"\u2713", 1, &checkRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return true;
}

void CALLBACK ModernMenuWinEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG objectId,
                                 LONG, DWORD, DWORD) {
    if ((event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) || !window || objectId != OBJID_WINDOW) return;
    wchar_t className[32]{};
    if (!GetClassNameW(window, className, ARRAYSIZE(className)) || wcscmp(className, L"#32768") != 0) return;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return;

    const LONG_PTR classStyle = GetClassLongPtrW(window, GCL_STYLE);
    const LONG_PTR desiredClassStyle = IsHighContrast() ? (classStyle | CS_DROPSHADOW)
                                                        : (classStyle & ~static_cast<LONG_PTR>(CS_DROPSHADOW));
    if (desiredClassStyle != classStyle) SetClassLongPtrW(window, GCL_STYLE, desiredClassStyle);
    if (event == EVENT_OBJECT_CREATE) return;

    const BOOL dark = g_app.theme == ThemeMode::Dark && !IsHighContrast();
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF border = IsHighContrast() ? DWMWA_COLOR_DEFAULT : DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
    if (!IsHighContrast()) {
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        if ((style & WS_BORDER) != 0) {
            SetWindowLongPtrW(window, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_BORDER));
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        RECT menuRect{};
        if (GetWindowRect(window, &menuRect)) {
            const int width = static_cast<int>(menuRect.right - menuRect.left);
            const int height = static_cast<int>(menuRect.bottom - menuRect.top);
            const UINT menuDpi = GetDpiForWindow(window);
            const int radius = MulDiv(10, static_cast<int>(menuDpi ? menuDpi : 96), 96);
            HRGN clip = CreateRoundRectRgn(1, 1, width - 1, height - 1, radius, radius);
            if (clip && SetWindowRgn(window, clip, TRUE) == 0) DeleteObject(clip);
        }
    }
}

UINT TrackModernPopupMenu(HMENU menu, UINT flags, int x, int y, HWND owner, LPTPMPARAMS parameters) {
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, ModernMenuWinEvent,
                                         GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    const UINT command = TrackPopupMenuEx(menu, flags, x, y, owner, parameters);
    if (hook) UnhookWinEvent(hook);
    return command;
}

LRESULT CALLBACK ToolbarButtonSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        if (g_app.hoveredToolbarButton != window) {
            HWND previous = g_app.hoveredToolbarButton; g_app.hoveredToolbarButton = window;
            if (previous) InvalidateRect(previous, nullptr, TRUE);
            InvalidateRect(window, nullptr, TRUE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        if (g_app.hoveredToolbarButton == window) g_app.hoveredToolbarButton = nullptr;
        InvalidateRect(window, nullptr, TRUE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ToolbarButtonSubclassProc, 1);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void ShowSettingsCategory(int category) {
    g_app.settingsCategory = (std::max)(0, (std::min)(category, 7));
    for (const SettingsControl& control : g_app.settingsControls)
        ShowWindow(control.window, control.category == g_app.settingsCategory ? SW_SHOW : SW_HIDE);
    if (g_app.settingsWindow) InvalidateRect(g_app.settingsWindow, nullptr, TRUE);
    for (int i = 0; i < 8; ++i) {
        HWND button = GetDlgItem(g_app.settingsWindow, ID_SETTINGS_CATEGORY_BASE + i);
        if (button) InvalidateRect(button, nullptr, TRUE);
    }
}

void UpdateSettingsLabels() {
    const struct { UINT id; const wchar_t* label; } toggles[] = {
        {ID_PREF_RESTORE, L"Restore folders at startup"}, {ID_PREF_CONFIRM_DELETE, L"Confirm permanent deletion"},
        {ID_PREF_STATUS, L"Show status bars"}, {ID_PREF_HIDDEN, L"Show hidden and system files"},
        {ID_PREF_DIRS_FIRST, L"Folders before files"}, {ID_PREF_EXTENSIONS, L"Show extension column"},
        {ID_PREF_CONTEXT_MENU, L"Use full Windows context menu"}, {ID_PREF_DRAG_MOVE, L"Move when dragging on same drive"},
        {ID_PREF_AUTO_REFRESH, L"Watch folders for changes"}, {ID_PREF_SIDEBAR_VISIBLE, L"Show navigation sidebar"},
        {ID_PREF_GALLERY, L"Show Gallery"}, {ID_PREF_RECYCLE, L"Show Recycle Bin"},
        {ID_PREF_NETWORK, L"Show Network"}, {ID_PREF_LINUX, L"Show Linux / WSL"},
        {ID_PREF_THEME_LIGHT, L"Light mode"}, {ID_PREF_THEME_DARK, L"Dark mode"}, {ID_PREF_DUAL, L"Dual-pane layout"},
        {ID_PREF_AUTO_UPDATES, L"Check automatically once per day"}
    };
    for (const auto& toggle : toggles) {
        HWND control = GetDlgItem(g_app.settingsWindow, toggle.id);
        if (!control) continue;
        SetWindowTextW(control, toggle.label);
        InvalidateRect(control, nullptr, TRUE);
    }
    HWND speed = GetDlgItem(g_app.settingsWindow, ID_PREF_REFRESH_SPEED);
    if (speed) {
        const std::wstring value = g_app.preferences.refreshMilliseconds == 1500 ? L"1.5" :
                                   std::to_wstring(g_app.preferences.refreshMilliseconds / 1000);
        const std::wstring text = L"Refresh interval    " + value + L" seconds";
        SetWindowTextW(speed, text.c_str());
        InvalidateRect(speed, nullptr, TRUE);
    }
}

HWND AddSettingsButton(HWND parent, UINT id, const wchar_t* text, int x, int y, int width, int height, int category = -1) {
    HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        Scale(x), Scale(y), Scale(width), Scale(height), parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g_app.instance, nullptr);
    ApplyFont(control, id >= ID_SETTINGS_CATEGORY_BASE && id < ID_SETTINGS_CATEGORY_BASE + 8);
    if (category >= 0) g_app.settingsControls.push_back({control, category});
    return control;
}

HWND AddSettingsText(HWND parent, const wchar_t* text, int x, int y, int width, int height, int category, bool bold = false) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        Scale(x), Scale(y), Scale(width), Scale(height), parent, nullptr, g_app.instance, nullptr);
    ApplyFont(control, bold); g_app.settingsControls.push_back({control, category}); return control;
}

void UpdateDualPaneButton() {
    if (g_app.commandButtons[7]) {
        SetWindowTextW(g_app.commandButtons[7], g_app.dualPane ? L"Dual pane" : L"Single pane");
        InvalidateRect(g_app.commandButtons[7], nullptr, TRUE);
    }
}

void ApplyPreferenceCommand(UINT id) {
    switch (id) {
    case ID_PREF_RESTORE: g_app.preferences.restorePaths = !g_app.preferences.restorePaths; break;
    case ID_PREF_CONFIRM_DELETE: g_app.preferences.confirmPermanentDelete = !g_app.preferences.confirmPermanentDelete; break;
    case ID_PREF_STATUS: g_app.preferences.showStatus = !g_app.preferences.showStatus; LayoutWindow(); break;
    case ID_PREF_HIDDEN: g_app.preferences.showHiddenSystem = !g_app.preferences.showHiddenSystem; RefreshAll(); break;
    case ID_PREF_DIRS_FIRST: g_app.preferences.directoriesFirst = !g_app.preferences.directoriesFirst; RefreshAll(); break;
    case ID_PREF_EXTENSIONS: g_app.preferences.showExtensions = !g_app.preferences.showExtensions; RefreshAll(); break;
    case ID_PREF_CONTEXT_MENU: g_app.preferences.shellContextMenu = !g_app.preferences.shellContextMenu; break;
    case ID_PREF_DRAG_MOVE: g_app.preferences.dragMoveSameDrive = !g_app.preferences.dragMoveSameDrive; break;
    case ID_PREF_AUTO_REFRESH: g_app.preferences.autoRefresh = !g_app.preferences.autoRefresh; break;
    case ID_PREF_REFRESH_SPEED:
        g_app.preferences.refreshMilliseconds = g_app.preferences.refreshMilliseconds <= 1500 ? 3000 :
                                                (g_app.preferences.refreshMilliseconds <= 3000 ? 5000 : 1500);
        KillTimer(g_app.window, 1); SetTimer(g_app.window, 1, g_app.preferences.refreshMilliseconds, nullptr); break;
    case ID_PREF_SIDEBAR_VISIBLE: g_app.preferences.sidebarVisible = !g_app.preferences.sidebarVisible; LayoutWindow(); break;
    case ID_PREF_GALLERY: g_app.preferences.sidebarGallery = !g_app.preferences.sidebarGallery; RebuildSidebar(); break;
    case ID_PREF_RECYCLE: g_app.preferences.sidebarRecycleBin = !g_app.preferences.sidebarRecycleBin; RebuildSidebar(); break;
    case ID_PREF_NETWORK: g_app.preferences.sidebarNetwork = !g_app.preferences.sidebarNetwork; RebuildSidebar(); break;
    case ID_PREF_LINUX: g_app.preferences.sidebarLinux = !g_app.preferences.sidebarLinux; RebuildSidebar(); break;
    case ID_PREF_THEME_LIGHT: g_app.theme = ThemeMode::Light; ApplyTheme(); break;
    case ID_PREF_THEME_DARK: g_app.theme = ThemeMode::Dark; ApplyTheme(); break;
    case ID_PREF_DUAL: g_app.dualPane = !g_app.dualPane; UpdateDualPaneButton(); LayoutWindow(); break;
    case ID_PREF_AUTO_UPDATES: g_app.preferences.automaticUpdates = !g_app.preferences.automaticUpdates; break;
    }
    UpdateSettingsLabels(); SaveSettings();
}

LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_app.settingsWindow = window; g_app.settingsControls.clear();
        const wchar_t* categories[] = {L"General", L"Appearance", L"Files", L"Operations", L"Navigation", L"Sidebar", L"Advanced", L"Updates"};
        for (int i = 0; i < 8; ++i) AddSettingsButton(window, ID_SETTINGS_CATEGORY_BASE + i, categories[i], 16, 70 + i * 39, 172, 34);
        const struct { int category; const wchar_t* title; } titles[] = {
            {0,L"General"},{1,L"Appearance"},{2,L"File display"},{3,L"File operations"},{4,L"Navigation and refresh"},{5,L"Windows shortcuts"},{6,L"Advanced"},{7,L"Updates"}};
        for (const auto& title : titles) AddSettingsText(window, title.title, 220, 72, 500, 30, title.category, true);
        AddSettingsButton(window, ID_PREF_RESTORE, L"", 220, 114, 500, 42, 0);
        AddSettingsButton(window, ID_PREF_CONFIRM_DELETE, L"", 220, 164, 500, 42, 0);
        AddSettingsButton(window, ID_PREF_DUAL, L"", 220, 214, 500, 42, 0);
        AddSettingsButton(window, ID_PREF_THEME_LIGHT, L"", 220, 114, 242, 42, 1);
        AddSettingsButton(window, ID_PREF_THEME_DARK, L"", 478, 114, 242, 42, 1);
        AddSettingsButton(window, ID_PREF_STATUS, L"", 220, 164, 500, 42, 1);
        AddSettingsButton(window, ID_PREF_HIDDEN, L"", 220, 114, 500, 42, 2);
        AddSettingsButton(window, ID_PREF_DIRS_FIRST, L"", 220, 164, 500, 42, 2);
        AddSettingsButton(window, ID_PREF_EXTENSIONS, L"", 220, 214, 500, 42, 2);
        AddSettingsButton(window, ID_PREF_CONTEXT_MENU, L"", 220, 114, 500, 42, 3);
        AddSettingsButton(window, ID_PREF_DRAG_MOVE, L"", 220, 164, 500, 42, 3);
        AddSettingsText(window, L"Copy, move, delete and rename use Windows IFileOperation with Undo support.", 220, 222, 500, 55, 3);
        AddSettingsButton(window, ID_PREF_AUTO_REFRESH, L"", 220, 114, 500, 42, 4);
        AddSettingsButton(window, ID_PREF_REFRESH_SPEED, L"", 220, 164, 500, 42, 4);
        AddSettingsText(window, L"Click the interval row to cycle through 1.5, 3 and 5 seconds.", 220, 222, 500, 40, 4);
        AddSettingsButton(window, ID_PREF_SIDEBAR_VISIBLE, L"", 220, 114, 500, 42, 5);
        AddSettingsButton(window, ID_PREF_GALLERY, L"", 220, 164, 242, 42, 5);
        AddSettingsButton(window, ID_PREF_RECYCLE, L"", 478, 164, 242, 42, 5);
        AddSettingsButton(window, ID_PREF_NETWORK, L"", 220, 214, 242, 42, 5);
        AddSettingsButton(window, ID_PREF_LINUX, L"", 478, 214, 242, 42, 5);
        AddSettingsText(window, L"Files4Me uses native Unicode paths, long-path aware APIs, Windows shell icons, the Recycle Bin, clipboard file transfer, drag and drop, keyboard shortcuts, and a portable INI file. No plug-ins or shell code is loaded unless the full context-menu option is enabled.", 220, 114, 500, 170, 6);
        AddSettingsButton(window, ID_PREF_AUTO_UPDATES, L"", 220, 114, 500, 42, 7);
        AddSettingsButton(window, ID_CHECK_UPDATES, L"Check for updates now", 220, 164, 500, 42, 7);
        AddSettingsText(window, L"Preview channel · signed manifests · GitHub Releases", 220, 222, 500, 40, 7);
        UpdateSettingsLabels(); ShowSettingsCategory(g_app.settingsCategory); ApplyTheme(); return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id >= ID_SETTINGS_CATEGORY_BASE && id < ID_SETTINGS_CATEGORY_BASE + 8) ShowSettingsCategory(id - ID_SETTINGS_CATEGORY_BASE);
        else if (id >= ID_PREF_RESTORE && id <= ID_PREF_DUAL) ApplyPreferenceCommand(id);
        else if (id == ID_PREF_AUTO_UPDATES) ApplyPreferenceCommand(id);
        else if (id == ID_CHECK_UPDATES) StartUpdateCheck(true);
        return 0;
    }
    case WM_DRAWITEM: DrawOwnerControl(reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, g_app.colors.text); SetBkColor(dc, g_app.colors.surface);
        return reinterpret_cast<LRESULT>(g_app.surfaceBrush);
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT client{}; GetClientRect(window, &client);
        FillRect(dc, &client, g_app.windowBrush);
        RECT title{Scale(18), Scale(10), client.right - Scale(18), Scale(38)};
        SelectObject(dc, g_app.fontBold); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, g_app.colors.text);
        DrawTextW(dc, L"Settings", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT subtitle{Scale(18), Scale(34), client.right - Scale(18), Scale(57)};
        SelectObject(dc, g_app.font); SetTextColor(dc, g_app.colors.muted);
        DrawTextW(dc, L"Personalize Files4Me", -1, &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        HBRUSH card = g_app.surfaceBrush; HGDIOBJ oldBrush = SelectObject(dc, card); HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, Scale(204), Scale(62), client.right - Scale(14), client.bottom - Scale(14), Scale(16), Scale(16));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); EndPaint(window, &paint); return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: g_app.settingsWindow = nullptr; g_app.settingsControls.clear(); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowSettingsWindow() {
    if (g_app.settingsWindow && IsWindow(g_app.settingsWindow)) {
        ShowWindow(g_app.settingsWindow, SW_SHOW); SetForegroundWindow(g_app.settingsWindow); return;
    }
    g_app.settingsWindow = nullptr;
    RECT owner{}; GetWindowRect(g_app.window, &owner);
    const int width = Scale(760), height = Scale(530);
    HWND settings = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass, L"Settings - Files4Me",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, owner.left + (owner.right-owner.left-width)/2,
        owner.top + (owner.bottom-owner.top-height)/2, width, height, g_app.window, nullptr, g_app.instance, nullptr);
    if (settings) { ShowWindow(settings, SW_SHOW); UpdateWindow(settings); }
    else {
        const DWORD error = GetLastError();
        wchar_t detail[96]{}; StringCchPrintfW(detail, ARRAYSIZE(detail), L"Unable to create Settings (error %lu).", error);
        MessageBoxW(g_app.window, detail, kAppName, MB_OK | MB_ICONERROR);
    }
}

void SetActivePane(int index) {
    if (index < 0 || index > 1 || g_app.activePane == index) return;
    g_app.activePane = index;
    const PaneMode mode = g_app.panes[index].mode;
    EnableWindow(g_app.commandButtons[2], mode == PaneMode::Filesystem || mode == PaneMode::Archive);
    EnableWindow(g_app.commandButtons[4], mode != PaneMode::DriveOverview);
    EnableWindow(g_app.commandButtons[6], mode == PaneMode::Filesystem);
    SetWindowTextW(g_app.commandButtons[4], mode == PaneMode::RecycleBin ? L"Empty Bin" :
                   mode == PaneMode::Archive ? L"Extract all" : L"New");
    UpdateSelectionCommands();
    InvalidateRect(g_app.window, nullptr, FALSE);
}

int PaneForId(UINT id) {
    if (id < ID_PANE_BASE) return -1;
    const int pane = static_cast<int>((id - ID_PANE_BASE) / ID_PANE_STRIDE);
    return pane >= 0 && pane < 2 ? pane : -1;
}

UINT LocalId(UINT id) { return (id - ID_PANE_BASE) % ID_PANE_STRIDE; }
UINT ControlId(int pane, UINT local) { return ID_PANE_BASE + pane * ID_PANE_STRIDE + local; }

void ShowDriveMenu(int paneIndex, HWND owner) {
    wchar_t drives[512]{};
    DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    if (length == 0 || length >= ARRAYSIZE(drives)) return;
    HMENU menu = CreatePopupMenu();
    std::vector<std::wstring> paths;
    for (const wchar_t* value = drives; *value; value += wcslen(value) + 1) {
        paths.emplace_back(value);
        wchar_t label[128]{};
        wchar_t volume[64]{};
        GetVolumeInformationW(value, volume, ARRAYSIZE(volume), nullptr, nullptr, nullptr, nullptr, 0);
        StringCchPrintfW(label, ARRAYSIZE(label), volume[0] ? L"%s  %s" : L"%s", value, volume);
        AppendMenuW(menu, MF_STRING, ID_DRIVE_BASE + static_cast<UINT>(paths.size() - 1), label);
    }
    RECT rect{}; GetWindowRect(owner, &rect);
    PrepareThemedMenu(menu);
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, g_app.window, nullptr);
    if (selected >= ID_DRIVE_BASE && selected < ID_DRIVE_BASE + paths.size()) Navigate(paneIndex, paths[selected - ID_DRIVE_BASE]);
    DestroyMenu(menu);
}

void SortPane(int paneIndex, SortColumn column) {
    Pane& pane = g_app.panes[paneIndex];
    if (pane.sort == column) pane.ascending = !pane.ascending;
    else { pane.sort = column; pane.ascending = true; }
    if (pane.timeline) {
        const std::vector<std::wstring> selected = SelectedPaths(paneIndex);
        RebuildTimelineRows(pane, selected);
        return;
    }
    std::sort(pane.sourceItems.begin(), pane.sourceItems.end(), [column, ascending = pane.ascending](const FileItem& a, const FileItem& b) {
        return CompareItems(a, b, column, ascending, g_app.preferences.directoriesFirst);
    });
    ApplyPaneFilter(paneIndex);
}

void RenameItem(int paneIndex, int itemIndex, const std::wstring& newName) {
    if (newName.empty() || newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBoxW(g_app.window, L"Name is empty or contains characters Windows does not allow.", kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    Pane& pane = g_app.panes[paneIndex];
    if (itemIndex < 0 || itemIndex >= static_cast<int>(pane.items.size())) return;
    EnqueueOperation(ID_RENAME, {pane.items[itemIndex].fullPath}, {}, newName);
}

ULONGLONG CurrentFileTimeValue() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{}; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime; return value.QuadPart;
}

template <typename T> void AppendJournalValue(std::vector<BYTE>& output, const T& value) {
    const BYTE* bytes = reinterpret_cast<const BYTE*>(&value); output.insert(output.end(), bytes, bytes + sizeof(T));
}

void AppendJournalString(std::vector<BYTE>& output, const std::wstring& value) {
    const uint32_t length = static_cast<uint32_t>((std::min<size_t>)(value.size(), 32767));
    AppendJournalValue(output, length);
    const BYTE* bytes = reinterpret_cast<const BYTE*>(value.data()); output.insert(output.end(), bytes, bytes + length * sizeof(wchar_t));
}

void SaveJobJournal() {
    if (g_app.jobsPath.empty()) return;
    std::vector<std::shared_ptr<OperationJob>> jobs;
    {
        std::lock_guard lock(g_app.operations.mutex);
        jobs = g_app.operations.jobs;
    }
    const ULONGLONG now = CurrentFileTimeValue();
    constexpr ULONGLONG thirtyDays = 30ULL * 24 * 60 * 60 * 10000000;
    std::erase_if(jobs, [&](const auto& job) {
        return job->state == JobState::Completed && job->created && now > job->created && now - job->created > thirtyDays;
    });
    if (jobs.size() > 100) jobs.erase(jobs.begin(), jobs.end() - 100);
    std::vector<BYTE> data;
    const uint32_t magic = 0x4A4D3446, version = 1, count = static_cast<uint32_t>(jobs.size());
    AppendJournalValue(data, magic); AppendJournalValue(data, version); AppendJournalValue(data, count);
    for (const auto& job : jobs) {
        const uint32_t kind = static_cast<uint32_t>(job->kind);
        uint32_t state = static_cast<uint32_t>(job->state.load());
        if (job->state == JobState::Running || job->state == JobState::Paused || job->state == JobState::Cancelling) state = static_cast<uint32_t>(JobState::Interrupted);
        const uint32_t conflict = static_cast<uint32_t>(job->conflict);
        const int32_t result = static_cast<int32_t>(job->result.load());
        AppendJournalValue(data, job->id); AppendJournalValue(data, kind); AppendJournalValue(data, state);
        AppendJournalValue(data, conflict); AppendJournalValue(data, result); AppendJournalValue(data, job->created);
        AppendJournalString(data, job->destination); AppendJournalString(data, job->newName);
        const uint32_t sourceCount = static_cast<uint32_t>((std::min<size_t>)(job->sources.size(), 4096));
        AppendJournalValue(data, sourceCount);
        for (uint32_t i = 0; i < sourceCount; ++i) AppendJournalString(data, job->sources[i]);
    }
    const std::wstring temporary = g_app.jobsPath + L".tmp";
    UniqueKernelHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return;
    DWORD written = 0; const BOOL okay = WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    const BOOL flushed = okay && FlushFileBuffers(file.get()); file.reset();
    if (flushed && written == data.size()) MoveFileExW(temporary.c_str(), g_app.jobsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    else DeleteFileW(temporary.c_str());
}

template <typename T> bool ReadJournalValue(const std::vector<BYTE>& data, size_t& offset, T& value) {
    if (offset > data.size() || sizeof(T) > data.size() - offset) return false;
    memcpy(&value, data.data() + offset, sizeof(T)); offset += sizeof(T); return true;
}

bool ReadJournalString(const std::vector<BYTE>& data, size_t& offset, std::wstring& value) {
    uint32_t length = 0;
    if (!ReadJournalValue(data, offset, length) || length > 32767 || length * sizeof(wchar_t) > data.size() - offset) return false;
    value.assign(reinterpret_cast<const wchar_t*>(data.data() + offset), length); offset += length * sizeof(wchar_t); return true;
}

void LoadJobJournal() {
    UniqueKernelHandle file(CreateFileW(g_app.jobsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 12 || size.QuadPart > 16 * 1024 * 1024) return;
    std::vector<BYTE> data(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
    const BOOL okay = ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr); file.reset();
    if (!okay || read != data.size()) return;
    size_t offset = 0; uint32_t magic = 0, version = 0, count = 0;
    if (!ReadJournalValue(data, offset, magic) || !ReadJournalValue(data, offset, version) || !ReadJournalValue(data, offset, count) ||
        magic != 0x4A4D3446 || version != 1 || count > 1000) return;
    std::vector<std::shared_ptr<OperationJob>> loaded;
    for (uint32_t index = 0; index < count; ++index) {
        auto job = std::make_shared<OperationJob>(); uint32_t kind = 0, state = 0, conflict = 0, sourceCount = 0; int32_t result = 0;
        if (!ReadJournalValue(data, offset, job->id) || !ReadJournalValue(data, offset, kind) || !ReadJournalValue(data, offset, state) ||
            !ReadJournalValue(data, offset, conflict) || !ReadJournalValue(data, offset, result) || !ReadJournalValue(data, offset, job->created) ||
            !ReadJournalString(data, offset, job->destination) || !ReadJournalString(data, offset, job->newName) ||
            !ReadJournalValue(data, offset, sourceCount) || sourceCount > 4096) return;
        job->kind = static_cast<JobKind>(kind); job->state = static_cast<JobState>(state); job->conflict = static_cast<ConflictPolicy>(conflict); job->result = result;
        for (uint32_t source = 0; source < sourceCount; ++source) { std::wstring path; if (!ReadJournalString(data, offset, path)) return; job->sources.push_back(std::move(path)); }
        if (job->state == JobState::Running || job->state == JobState::Paused || job->state == JobState::Cancelling) job->state = JobState::Interrupted;
        for (const auto& source : job->sources) job->resourceKeys.push_back(OperationResourceKey(source));
        if (!job->destination.empty()) job->resourceKeys.push_back(OperationResourceKey(job->destination));
        std::sort(job->resourceKeys.begin(), job->resourceKeys.end()); job->resourceKeys.erase(std::unique(job->resourceKeys.begin(), job->resourceKeys.end()), job->resourceKeys.end());
        loaded.push_back(job); g_app.operations.nextId = (std::max)(g_app.operations.nextId, job->id + 1);
    }
    std::lock_guard lock(g_app.operations.mutex); g_app.operations.jobs = std::move(loaded);
}

bool DirectoryWritable(const std::wstring& directory) {
    const std::wstring test = JoinPath(directory, L".files4me-write-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    UniqueKernelHandle file(CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!file) return false;
    file.reset(); DeleteFileW(test.c_str()); return true;
}

std::wstring ResolveStateDirectory() {
    const std::wstring portable = ExecutableDirectory();
    if (GetFileAttributesW(JoinPath(portable, L"installed.marker").c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring local = KnownFolderPath(FOLDERID_LocalAppData, portable.c_str());
        const std::wstring folder = JoinPath(local, L"Files4Me");
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder;
    }
    if (DirectoryWritable(portable)) return portable;
    const std::wstring local = KnownFolderPath(FOLDERID_LocalAppData, portable.c_str());
    const std::wstring folder = JoinPath(local, L"Files4Me");
    CreateDirectoryW(folder.c_str(), nullptr); return folder;
}

void SaveSettings() {
    WritePrivateProfileStringW(L"Appearance", L"Theme", g_app.theme == ThemeMode::Dark ? L"Dark" : L"Light", g_app.iniPath.c_str());
    WritePrivateProfileStringW(L"Layout", L"DualPane", g_app.dualPane ? L"1" : L"0", g_app.iniPath.c_str());
    WritePrivateProfileStringW(L"Layout", L"ActivePane", g_app.activePane ? L"1" : L"0", g_app.iniPath.c_str());
    const auto writeBool = [](const wchar_t* section, const wchar_t* key, bool value) {
        WritePrivateProfileStringW(section, key, value ? L"1" : L"0", g_app.iniPath.c_str());
    };
    writeBool(L"Sidebar", L"Visible", g_app.preferences.sidebarVisible);
    writeBool(L"Sidebar", L"Gallery", g_app.preferences.sidebarGallery);
    writeBool(L"Sidebar", L"RecycleBin", g_app.preferences.sidebarRecycleBin);
    writeBool(L"Sidebar", L"Network", g_app.preferences.sidebarNetwork);
    writeBool(L"Sidebar", L"Linux", g_app.preferences.sidebarLinux);
    writeBool(L"General", L"RestorePaths", g_app.preferences.restorePaths);
    writeBool(L"General", L"ConfirmPermanentDelete", g_app.preferences.confirmPermanentDelete);
    writeBool(L"Display", L"ShowHiddenSystem", g_app.preferences.showHiddenSystem);
    writeBool(L"Display", L"DirectoriesFirst", g_app.preferences.directoriesFirst);
    writeBool(L"Display", L"ShowExtensions", g_app.preferences.showExtensions);
    writeBool(L"Display", L"ShowStatus", g_app.preferences.showStatus);
    writeBool(L"Navigation", L"AutoRefresh", g_app.preferences.autoRefresh);
    writeBool(L"Operations", L"ShellContextMenu", g_app.preferences.shellContextMenu);
    writeBool(L"Operations", L"DragMoveSameDrive", g_app.preferences.dragMoveSameDrive);
    writeBool(L"Updates", L"AutomaticChecks", g_app.preferences.automaticUpdates);
    WritePrivateProfileStringW(L"SidebarPins", L"Count", std::to_wstring(g_app.pinnedFolders.size()).c_str(), g_app.iniPath.c_str());
    for (size_t index = 0; index < g_app.pinnedFolders.size(); ++index) {
        const std::wstring key = L"Path" + std::to_wstring(index);
        WritePrivateProfileStringW(L"SidebarPins", key.c_str(), g_app.pinnedFolders[index].c_str(), g_app.iniPath.c_str());
    }
    WritePrivateProfileStringW(L"Navigation", L"RefreshMilliseconds",
                               std::to_wstring(g_app.preferences.refreshMilliseconds).c_str(), g_app.iniPath.c_str());
    for (int i = 0; i < 2; ++i) {
        const wchar_t* section = i == 0 ? L"LeftPane" : L"RightPane";
        const Pane& pane = g_app.panes[i];
        if (!pane.tabs.empty()) WritePrivateProfileStringW(section, L"Path", pane.tabs[pane.activeTab].path.c_str(), g_app.iniPath.c_str());
        WritePrivateProfileStringW(section, L"View", std::to_wstring(static_cast<int>(pane.view)).c_str(), g_app.iniPath.c_str());
    }
    RECT rect{};
    if (GetWindowRect(g_app.window, &rect) && !IsIconic(g_app.window) && !IsZoomed(g_app.window)) {
        WritePrivateProfileStringW(L"Window", L"X", std::to_wstring(rect.left).c_str(), g_app.iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Y", std::to_wstring(rect.top).c_str(), g_app.iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Width", std::to_wstring(rect.right - rect.left).c_str(), g_app.iniPath.c_str());
        WritePrivateProfileStringW(L"Window", L"Height", std::to_wstring(rect.bottom - rect.top).c_str(), g_app.iniPath.c_str());
    }
}

void LoadPreferences() {
    const auto readBool = [](const wchar_t* section, const wchar_t* key, bool fallback) {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_app.iniPath.c_str()) != 0;
    };
    g_app.preferences.sidebarVisible = readBool(L"Sidebar", L"Visible", true);
    g_app.preferences.sidebarGallery = readBool(L"Sidebar", L"Gallery", true);
    g_app.preferences.sidebarRecycleBin = readBool(L"Sidebar", L"RecycleBin", true);
    g_app.preferences.sidebarNetwork = readBool(L"Sidebar", L"Network", true);
    g_app.preferences.sidebarLinux = readBool(L"Sidebar", L"Linux", true);
    g_app.preferences.restorePaths = readBool(L"General", L"RestorePaths", true);
    g_app.preferences.confirmPermanentDelete = readBool(L"General", L"ConfirmPermanentDelete", true);
    g_app.preferences.showHiddenSystem = readBool(L"Display", L"ShowHiddenSystem", false);
    g_app.preferences.directoriesFirst = readBool(L"Display", L"DirectoriesFirst", true);
    g_app.preferences.showExtensions = readBool(L"Display", L"ShowExtensions", true);
    g_app.preferences.showStatus = readBool(L"Display", L"ShowStatus", true);
    g_app.preferences.autoRefresh = readBool(L"Navigation", L"AutoRefresh", true);
    g_app.preferences.shellContextMenu = readBool(L"Operations", L"ShellContextMenu", true);
    g_app.preferences.dragMoveSameDrive = readBool(L"Operations", L"DragMoveSameDrive", true);
    g_app.preferences.automaticUpdates = readBool(L"Updates", L"AutomaticChecks", true);
    g_app.pinnedFolders.clear();
    const UINT pinnedCount = (std::min)(GetPrivateProfileIntW(L"SidebarPins", L"Count", 0, g_app.iniPath.c_str()), 32U);
    for (UINT index = 0; index < pinnedCount; ++index) {
        const std::wstring key = L"Path" + std::to_wstring(index);
        std::array<wchar_t, 32768> path{};
        GetPrivateProfileStringW(L"SidebarPins", key.c_str(), L"", path.data(), static_cast<DWORD>(path.size()), g_app.iniPath.c_str());
        if (path[0] && !IsPinnedFolder(path.data())) g_app.pinnedFolders.emplace_back(path.data());
    }
    const UINT interval = GetPrivateProfileIntW(L"Navigation", L"RefreshMilliseconds", 1500, g_app.iniPath.c_str());
    g_app.preferences.refreshMilliseconds = interval == 3000 || interval == 5000 ? interval : 1500;
}

std::wstring ReadSetting(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    std::array<wchar_t, 32768> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), g_app.iniPath.c_str());
    return buffer.data();
}

void StartUpdateCheck(bool manual) {
    if (g_app.updateChecking.exchange(true)) {
        if (manual) MessageBoxW(g_app.window, L"An update check is already running.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!manual) {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"Updates", L"LastSuccessfulCheck", L"0", value, ARRAYSIZE(value), g_app.iniPath.c_str());
        const ULONGLONG last = _wcstoui64(value, nullptr, 10), now = CurrentFileTimeValue();
        constexpr ULONGLONG oneDay = 24ULL * 60 * 60 * 10000000;
        if (last && now >= last && now - last < oneDay) { g_app.updateChecking = false; return; }
    }
    BeginUpdateCheck(g_app.window, WM_APP_UPDATE_DONE, manual);
}

void OpenTrustedWebPage(const std::wstring& url) {
    ShellExecuteW(g_app.window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HandleUpdateResult(std::unique_ptr<UpdateResult> result) {
    g_app.updateChecking = false;
    if (!result) return;
    if (result->status == UpdateStatus::UpToDate || result->status == UpdateStatus::Available) {
        WritePrivateProfileStringW(L"Updates", L"LastSuccessfulCheck",
                                   std::to_wstring(CurrentFileTimeValue()).c_str(), g_app.iniPath.c_str());
    }
    if (result->status == UpdateStatus::UpToDate) {
        if (result->manual) MessageBoxW(g_app.window, L"Files4Me is up to date.", L"Check for updates", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (result->status == UpdateStatus::Available) {
        std::wstring prompt = L"Files4Me " + result->manifest.displayVersion + L" is available.\n\n" + result->manifest.notes;
        if (!IsFiles4MeInstalledBuild()) {
            prompt += L"\n\nThis is a portable copy. Open the GitHub release page?";
            if (MessageBoxW(g_app.window, prompt.c_str(), L"Files4Me update", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                OpenTrustedWebPage(result->manifest.releaseUrl);
            return;
        }
        prompt += L"\n\nYes: download and install\nNo: view release page\nCancel: later";
        const int choice = MessageBoxW(g_app.window, prompt.c_str(), L"Files4Me update", MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (choice == IDNO) { OpenTrustedWebPage(result->manifest.releaseUrl); return; }
        if (choice == IDYES) {
            g_app.updateChecking = true;
            BeginUpdateDownload(g_app.window, WM_APP_UPDATE_DONE, result->manifest, g_app.stateDirectory);
        }
        return;
    }
    if (result->status == UpdateStatus::DownloadReady) {
        size_t active = 0; { std::lock_guard lock(g_app.operations.mutex); active = g_app.operations.activeCount; }
        if (active) {
            MessageBoxW(g_app.window, L"The update is verified and ready, but file operations are active. Finish them and check again to install.",
                        L"Files4Me update", MB_OK | MB_ICONINFORMATION); return;
        }
        if (MessageBoxW(g_app.window, L"The update was downloaded and verified. Install it now? Files4Me will close.",
                        L"Files4Me update", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            SHELLEXECUTEINFOW execute{sizeof(execute)}; execute.hwnd = g_app.window; execute.lpVerb = L"open";
            execute.lpFile = result->downloadedPath.c_str(); execute.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&execute)) PostMessageW(g_app.window, WM_CLOSE, 0, 0);
            else ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Launch updater");
        }
        return;
    }
    if (result->manual) {
        const std::wstring detail = result->detail.empty() ? L"The update check failed." : result->detail;
        MessageBoxW(g_app.window, detail.c_str(), L"Check for updates", MB_OK | MB_ICONWARNING);
    }
}

HWND CreateOwnerButton(HWND parent, UINT id, const wchar_t* text) {
    HWND button = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g_app.instance, nullptr);
    ApplyFont(button);
    return button;
}

LRESULT CALLBACK LayoutFlyoutProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        const UINT ids[] = {ID_VIEW_EXTRA_LARGE, ID_VIEW_LARGE, ID_VIEW_MEDIUM, ID_VIEW_SMALL,
                            ID_VIEW_LIST, ID_VIEW_DETAILS, ID_VIEW_TILES, ID_VIEW_CONTENT};
        for (size_t i = 0; i < g_app.layoutButtons.size(); ++i) {
            g_app.layoutButtons[i] = CreateOwnerButton(window, ids[i], ViewModeName(ViewModeFromCommand(ids[i])));
        }
        g_app.layoutCaption = CreateWindowExW(0, WC_STATICW, L"Layout", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                               0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        ApplyFont(g_app.layoutCaption);
        return 0;
    }
    case WM_SIZE: {
        const int pad = Scale(8), gap = Scale(4), rowHeight = Scale(32), captionHeight = Scale(24);
        RECT client{}; GetClientRect(window, &client);
        const int columnWidth = (client.right - pad * 2 - gap * 2) / 3;
        const int positions[][2] = {{0,0},{1,0},{2,0},{0,1},{1,1},{2,1},{0,2},{1,2}};
        for (size_t i = 0; i < g_app.layoutButtons.size(); ++i) {
            const int column = positions[i][0], row = positions[i][1];
            MoveWindow(g_app.layoutButtons[i], pad + column * (columnWidth + gap), pad + row * (rowHeight + gap),
                       columnWidth, rowHeight, TRUE);
        }
        MoveWindow(g_app.layoutCaption, pad, client.bottom - captionHeight - Scale(4), client.right - pad * 2, captionHeight, TRUE);
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (IsViewCommand(id)) {
            ApplyPaneView(g_app.activePane, ViewModeFromCommand(id));
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_DRAWITEM:
        DrawOwnerControl(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, g_app.colors.muted); SetBkColor(dc, g_app.colors.surface);
        return reinterpret_cast<LRESULT>(g_app.surfaceBrush);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, g_app.surfaceBrush);
        HPEN pen = CreatePen(PS_SOLID, 1, g_app.colors.border);
        HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, client.right, client.bottom, Scale(10), Scale(10));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        EndPaint(window, &paint); return 0;
    }
    case WM_NCDESTROY:
        g_app.layoutPopup = nullptr; g_app.layoutCaption = nullptr; g_app.layoutButtons.fill(nullptr);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowLayoutFlyout() {
    if (g_app.layoutPopup && IsWindow(g_app.layoutPopup)) { DestroyWindow(g_app.layoutPopup); return; }
    RECT anchor{}; GetWindowRect(g_app.commandButtons[6], &anchor);
    const int width = Scale(420), height = Scale(144);
    RECT desired{anchor.right - width, anchor.bottom + Scale(6), anchor.right, anchor.bottom + Scale(6) + height};
    HMONITOR monitor = MonitorFromRect(&desired, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
    if (desired.right > info.rcWork.right) OffsetRect(&desired, info.rcWork.right - desired.right, 0);
    if (desired.left < info.rcWork.left) OffsetRect(&desired, info.rcWork.left - desired.left, 0);
    if (desired.bottom > info.rcWork.bottom) OffsetRect(&desired, 0, anchor.top - Scale(6) - desired.bottom);
    g_app.layoutPopup = CreateWindowExW(WS_EX_TOOLWINDOW, kLayoutClass, L"Layout", WS_POPUP,
                                         desired.left, desired.top, width, height, g_app.window, nullptr, g_app.instance, nullptr);
    if (!g_app.layoutPopup) return;
    const BOOL dark = g_app.theme == ThemeMode::Dark;
    const int rounded = 2;
    DwmSetWindowAttribute(g_app.layoutPopup, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_app.layoutPopup, static_cast<DWMWINDOWATTRIBUTE>(33), &rounded, sizeof(rounded));
    ShowWindow(g_app.layoutPopup, SW_SHOW);
    SetForegroundWindow(g_app.layoutPopup);
    const size_t selected = static_cast<size_t>(g_app.panes[g_app.activePane].view);
    if (selected < g_app.layoutButtons.size()) SetFocus(g_app.layoutButtons[selected]);
}

void CreatePane(int index) {
    Pane& pane = g_app.panes[index];
    pane.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
                               0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ControlId(index, ID_TAB))), g_app.instance, nullptr);
    SetWindowSubclass(pane.tab, TabSubclassProc, 1, static_cast<DWORD_PTR>(index));
    pane.drives = CreateOwnerButton(g_app.window, ControlId(index, ID_DRIVES), L"Drives");
    pane.path = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ControlId(index, ID_PATH))), g_app.instance, nullptr);
    SetWindowSubclass(pane.path, PathEditSubclassProc, 3, static_cast<DWORD_PTR>(index));
    pane.search = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ControlId(index, ID_SEARCH))), g_app.instance, nullptr);
    SendMessageW(pane.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter this folder"));
    const wchar_t* labels[] = {L"Name", L"Ext", L"Size", L"Modified"};
    for (int i = 0; i < 4; ++i) {
        pane.headers[i] = CreateOwnerButton(g_app.window, ControlId(index, ID_HEADER_NAME + i), labels[i]);
        SetWindowSubclass(pane.headers[i], HeaderSubclassProc, 4,
                          static_cast<DWORD_PTR>(index * 4 + i));
    }
    pane.list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS,
                               0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ControlId(index, ID_LIST))), g_app.instance, nullptr);
    pane.status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ControlId(index, ID_STATUS))), g_app.instance, nullptr);
    ApplyFont(pane.tab); ApplyFont(pane.path); ApplyFont(pane.search); ApplyFont(pane.status);
    SendMessageW(pane.list, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fileFont), TRUE);
    TabCtrl_SetItemSize(pane.tab, Scale(150), Scale(29));
    ListView_SetExtendedListViewStyle(pane.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* columnNames[] = {L"Name", L"Extension", L"Size", L"Modified"};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = const_cast<wchar_t*>(columnNames[i]);
        column.cx = Scale(120); column.fmt = (i == 2) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(pane.list, i, &column);
    }
    ListView_SetImageList(pane.list, g_app.smallIconCache.images, LVSIL_SMALL);
    SetWindowSubclass(pane.list, ListSubclassProc, 2, static_cast<DWORD_PTR>(index));
    auto* dropTarget = new (std::nothrow) FileDropTarget(index);
    if (dropTarget) {
        if (SUCCEEDED(RegisterDragDrop(pane.list, dropTarget))) pane.dropTarget = dropTarget;
        else dropTarget->Release();
    }

    std::wstring section = index == 0 ? L"LeftPane" : L"RightPane";
    pane.view = ParseViewMode(ReadSetting(section.c_str(), L"View", L"5"));
    std::wstring path = g_app.preferences.restorePaths ? ReadSetting(section.c_str(), L"Path", DefaultPath().c_str()) : DefaultPath();
    if (!IsVirtualLocation(path) && GetFileAttributesW(ExtendedPath(path).c_str()) == INVALID_FILE_ATTRIBUTES) path = DefaultPath();
    pane.tabs.push_back({path});
    TCITEMW tabItem{}; tabItem.mask = TCIF_TEXT;
    std::wstring text = DisplayNameForPath(path); tabItem.pszText = text.data();
    TabCtrl_InsertItem(pane.tab, 0, &tabItem);
    SetWindowTextW(pane.path, path.c_str());
}

void ShowFallbackContextMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open\tEnter");
    if (!SelectedZipPath(g_app.activePane).empty())
        AppendMenuW(menu, MF_STRING, ID_EXTRACT_ALL, L"Extract all");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_COPY, L"Copy to other pane\tF5");
    AppendMenuW(menu, MF_STRING, ID_MOVE, L"Move to other pane\tF6");
    AppendMenuW(menu, MF_STRING, ID_RENAME, L"Rename\tF2");
    AppendMenuW(menu, MF_STRING, ID_DELETE, L"Recycle\tDelete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_CLIP_COPY, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING, ID_CLIP_CUT, L"Cut\tCtrl+X");
    AppendMenuW(menu, MF_STRING, ID_CLIP_PASTE, L"Paste\tCtrl+V");
    PrepareThemedMenu(menu);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, g_app.window, nullptr);
    DestroyMenu(menu);
    if (command) SendMessageW(g_app.window, WM_COMMAND, command, 0);
}

void ShowBackgroundContextMenu(int paneIndex, POINT point) {
    Pane& pane = g_app.panes[paneIndex];
    if (pane.mode != PaneMode::Filesystem || pane.tabs.empty()) return;
    ListView_SetItemState(pane.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    UpdateSelectionCommands();

    HMENU menu = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU sort = CreatePopupMenu();
    HMENU newItems = BuildNewItemsMenu();
    if (!menu || !view || !sort || !newItems) {
        if (menu) DestroyMenu(menu); else { if (view) DestroyMenu(view); if (sort) DestroyMenu(sort); if (newItems) DestroyMenu(newItems); }
        return;
    }
    for (UINT viewId = ID_VIEW_EXTRA_LARGE; viewId <= ID_VIEW_CONTENT; ++viewId) {
        const UINT checked = CommandFromViewMode(pane.view) == viewId ? MF_CHECKED : 0;
        AppendMenuW(view, MF_STRING | checked, viewId, ViewModeName(ViewModeFromCommand(viewId)));
    }
    AppendMenuW(sort, MF_STRING | (pane.sort == SortColumn::Name ? MF_CHECKED : 0), ID_SORT_NAME, L"Name");
    AppendMenuW(sort, MF_STRING | (pane.sort == SortColumn::Extension ? MF_CHECKED : 0), ID_SORT_EXTENSION, L"Type");
    AppendMenuW(sort, MF_STRING | (pane.sort == SortColumn::Size ? MF_CHECKED : 0), ID_SORT_SIZE, L"Size");
    AppendMenuW(sort, MF_STRING | (pane.sort == SortColumn::Modified ? MF_CHECKED : 0), ID_SORT_MODIFIED, L"Date modified");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort), L"Sort by");
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsClipboardFormatAvailable(CF_HDROP) ? 0 : MF_GRAYED), ID_CLIP_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(newItems), L"New");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_FOLDER_PROPERTIES, L"Properties");
    PrepareThemedMenu(menu);
    const UINT command = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                               point.x, point.y, g_app.window);
    DestroyMenu(menu);
    if (command) SendMessageW(g_app.window, WM_COMMAND, command, 0);
}

void ShowContextMenu(int paneIndex, POINT point) {
    SetActivePane(paneIndex);
    if (g_app.panes[paneIndex].mode == PaneMode::RecycleBin) {
        HMENU menu = CreatePopupMenu();
        const bool selected = ListView_GetSelectedCount(g_app.panes[paneIndex].list) > 0;
        AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), ID_RECYCLE_RESTORE, L"Restore");
        AppendMenuW(menu, MF_STRING, ID_RECYCLE_RESTORE_ALL, L"Restore all");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), ID_DELETE_PERMANENT, L"Delete permanently");
        AppendMenuW(menu, MF_STRING, ID_RECYCLE_EMPTY, L"Empty Recycle Bin");
        PrepareThemedMenu(menu);
        const UINT command = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, g_app.window);
        DestroyMenu(menu); if (command) SendMessageW(g_app.window, WM_COMMAND, command, 0);
        return;
    }
    if (g_app.panes[paneIndex].mode == PaneMode::Archive) {
        HMENU menu = CreatePopupMenu();
        const size_t count = SelectedArchiveEntries(paneIndex).size();
        AppendMenuW(menu, MF_STRING | (count == 1 ? 0 : MF_GRAYED), ID_OPEN, L"Open");
        AppendMenuW(menu, MF_STRING | (count ? 0 : MF_GRAYED), ID_CLIP_COPY, L"Copy");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXTRACT_ALL, L"Extract all");
        PrepareThemedMenu(menu);
        const UINT command = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                  point.x, point.y, g_app.window);
        DestroyMenu(menu); if (command) SendMessageW(g_app.window, WM_COMMAND, command, 0); return;
    }
    if (g_app.panes[paneIndex].mode != PaneMode::Filesystem) return;
    if (!g_app.preferences.shellContextMenu) { ShowFallbackContextMenu(point); return; }
    const std::vector<std::wstring> paths = SelectedPaths(paneIndex);
    ShellSelection selection;
    if (!BuildShellSelection(paths, selection)) { ShowFallbackContextMenu(point); return; }

    ComPtr<IContextMenu> contextMenu;
    HRESULT result = selection.parent->GetUIObjectOf(g_app.window,
        static_cast<UINT>(selection.childPidls.size()), selection.childPidls.data(),
        IID_IContextMenu, nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(result) || !contextMenu) { ShowFallbackContextMenu(point); return; }

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    UINT flags = CMF_NORMAL | CMF_CANRENAME | CMF_ITEMMENU;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) flags |= CMF_EXTENDEDVERBS;
    result = contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, flags);
    if (FAILED(result)) { DestroyMenu(menu); ShowFallbackContextMenu(point); return; }
    if (!SelectedZipPath(paneIndex).empty()) {
        InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, ID_EXTRACT_ALL, L"Extract all");
        InsertMenuW(menu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    contextMenu.As(&g_app.activeContextMenu3);
    contextMenu.As(&g_app.activeContextMenu2);
    SetForegroundWindow(g_app.window);
    const UINT command = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              point.x, point.y, g_app.window);
    PostMessageW(g_app.window, WM_NULL, 0, 0);
    g_app.activeContextMenu3.Reset();
    g_app.activeContextMenu2.Reset();
    DestroyMenu(menu);
    if (command == 0) return;
    if (command == ID_EXTRACT_ALL) { SendMessageW(g_app.window, WM_COMMAND, ID_EXTRACT_ALL, 0); return; }

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    invoke.hwnd = g_app.window;
    invoke.lpVerb = MAKEINTRESOURCEA(command - 1);
    invoke.lpVerbW = MAKEINTRESOURCEW(command - 1);
    invoke.nShow = SW_SHOWNORMAL;
    invoke.ptInvoke = point;
    result = contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    ShowError(result, L"Shell command");
    RefreshAll();
}

void CopySelectedPathsAsText() {
    const auto paths = SelectedPaths(g_app.activePane);
    if (paths.empty()) return;
    std::wstring text;
    for (size_t index = 0; index < paths.size(); ++index) {
        if (index) text += L"\r\n";
        text += L'"'; text += paths[index]; text += L'"';
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return;
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return; }
    memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(g_app.window)) { GlobalFree(memory); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    CloseClipboard();
}

void InvokeSelectionShellVerb(const char* verb, const wchar_t* verbWide) {
    const auto paths = SelectedPaths(g_app.activePane);
    ShellSelection selection;
    if (!BuildShellSelection(paths, selection)) return;
    ComPtr<IContextMenu> contextMenu;
    HRESULT result = selection.parent->GetUIObjectOf(g_app.window,
        static_cast<UINT>(selection.childPidls.size()), selection.childPidls.data(),
        IID_IContextMenu, nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(result) || !contextMenu) { ShowError(result, L"Shell command"); return; }
    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = g_app.window;
    invoke.lpVerb = verb;
    invoke.lpVerbW = verbWide;
    invoke.nShow = SW_SHOWNORMAL;
    result = contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    ShowError(result, L"Shell command");
}

bool InvokeRecycleVerb(const char* verb, const wchar_t* verbWide, bool allItems) {
    Pane& pane = g_app.panes[g_app.activePane];
    if (pane.mode != PaneMode::RecycleBin) return false;
    std::vector<ComPtr<IShellItem2>> chosen;
    for (int index = 0; index < static_cast<int>(pane.recycleItems.size()); ++index) {
        if (allItems || (ListView_GetItemState(pane.list, index, LVIS_SELECTED) & LVIS_SELECTED))
            chosen.push_back(pane.recycleItems[index].shellItem);
    }
    if (chosen.empty()) return false;
    std::vector<PIDLIST_ABSOLUTE> absolute;
    std::vector<PCUITEMID_CHILD> children;
    ComPtr<IShellFolder> parent;
    for (const auto& item : chosen) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHGetIDListFromObject(item.Get(), &pidl)) || !pidl) continue;
        ComPtr<IShellFolder> itemParent;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(&itemParent), &child)) || !child) {
            CoTaskMemFree(pidl); continue;
        }
        if (!parent) parent = itemParent;
        absolute.push_back(pidl); children.push_back(child);
    }
    if (!parent || children.empty()) {
        for (auto pidl : absolute) CoTaskMemFree(pidl);
        return false;
    }
    ComPtr<IContextMenu> menu;
    HRESULT result = parent->GetUIObjectOf(g_app.window, static_cast<UINT>(children.size()), children.data(),
                                            IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.GetAddressOf()));
    if (SUCCEEDED(result) && menu) {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke); invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = g_app.window; invoke.lpVerb = verb; invoke.lpVerbW = verbWide; invoke.nShow = SW_SHOWNORMAL;
        result = menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    }
    for (auto pidl : absolute) CoTaskMemFree(pidl);
    if (FAILED(result)) ShowError(result, L"Recycle Bin command");
    else LoadRecycleBin(g_app.activePane);
    return SUCCEEDED(result);
}

void EmptyRecycleBin() {
    if (MessageBoxW(g_app.window, L"Permanently delete all items in the Recycle Bin?", kAppName,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const HRESULT result = SHEmptyRecycleBinW(g_app.window, nullptr, SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    if (FAILED(result)) ShowError(result, L"Empty Recycle Bin");
    LoadRecycleBin(g_app.activePane);
}

void ShowSelectionMoreMenu() {
    if (g_app.panes[g_app.activePane].mode == PaneMode::RecycleBin) {
        HMENU recycleMenu = CreatePopupMenu();
        const bool selectedItems = ListView_GetSelectedCount(g_app.panes[g_app.activePane].list) > 0;
        AppendMenuW(recycleMenu, MF_STRING | (selectedItems ? 0 : MF_GRAYED), ID_RECYCLE_RESTORE, L"Restore");
        AppendMenuW(recycleMenu, MF_STRING, ID_RECYCLE_RESTORE_ALL, L"Restore all");
        AppendMenuW(recycleMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(recycleMenu, MF_STRING | (selectedItems ? 0 : MF_GRAYED), ID_DELETE_PERMANENT, L"Delete permanently");
        AppendMenuW(recycleMenu, MF_STRING, ID_RECYCLE_EMPTY, L"Empty Recycle Bin");
        RECT rect{}; GetWindowRect(g_app.selectionButtons[5], &rect);
        PrepareThemedMenu(recycleMenu);
        const UINT selected = TrackModernPopupMenu(recycleMenu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                                   rect.right, rect.bottom, g_app.window);
        DestroyMenu(recycleMenu);
        if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
        return;
    }
    if (g_app.panes[g_app.activePane].mode == PaneMode::Archive) {
        HMENU archiveMenu = CreatePopupMenu();
        const size_t count = SelectedArchiveEntries(g_app.activePane).size();
        AppendMenuW(archiveMenu, MF_STRING | (count == 1 ? 0 : MF_GRAYED), ID_OPEN, L"Open\tEnter");
        AppendMenuW(archiveMenu, MF_STRING | (count ? 0 : MF_GRAYED), ID_CLIP_COPY, L"Copy\tCtrl+C");
        AppendMenuW(archiveMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(archiveMenu, MF_STRING, ID_EXTRACT_ALL, L"Extract all");
        RECT rect{}; GetWindowRect(g_app.selectionButtons[5], &rect); PrepareThemedMenu(archiveMenu);
        const UINT selected = TrackModernPopupMenu(archiveMenu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                                   rect.right, rect.bottom, g_app.window);
        DestroyMenu(archiveMenu); if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0); return;
    }
    const auto paths = SelectedPaths(g_app.activePane);
    const bool hasSelection = !paths.empty();
    const bool oneItem = paths.size() == 1;
    const DWORD attributes = oneItem ? GetFileAttributesW(ExtendedPath(paths.front()).c_str()) : INVALID_FILE_ATTRIBUTES;
    const bool oneFile = oneItem && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    const bool validOtherPane = g_app.dualPane && !g_app.panes[1 - g_app.activePane].tabs.empty();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (oneItem ? 0 : MF_GRAYED), ID_OPEN, L"Open\tEnter");
    AppendMenuW(menu, MF_STRING | (oneFile ? 0 : MF_GRAYED), ID_OPEN_WITH, L"Open with...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_CLIP_CUT, L"Cut\tCtrl+X");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_CLIP_COPY, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING | (IsClipboardFormatAvailable(CF_HDROP) && ActiveFolderAcceptsPaste() ? 0 : MF_GRAYED),
                ID_CLIP_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(menu, MF_STRING | (oneItem ? 0 : MF_GRAYED), ID_RENAME, L"Rename\tF2");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_DELETE, L"Move to Recycle Bin\tDelete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (validOtherPane ? 0 : MF_GRAYED), ID_COPY, L"Copy to other pane\tF5");
    AppendMenuW(menu, MF_STRING | (validOtherPane ? 0 : MF_GRAYED), ID_MOVE, L"Move to other pane\tF6");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_COPY_PATH, paths.size() == 1 ? L"Copy path" : L"Copy paths");
    const bool oneFolder = oneItem && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    AppendMenuW(menu, MF_STRING | (oneFolder && !IsPinnedFolder(paths.front()) ? 0 : MF_GRAYED), ID_PIN_SIDEBAR, L"Pin to sidebar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_PROPERTIES, L"Properties");
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_DELETE_PERMANENT, L"Delete permanently\tShift+Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), ID_SHELL_CONTEXT, L"Show more options");

    RECT rect{};
    GetWindowRect(g_app.selectionButtons[5], &rect);
    SetForegroundWindow(g_app.window);
    PrepareThemedMenu(menu);
    const UINT selected = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                               rect.right, rect.bottom, g_app.window);
    PostMessageW(g_app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
}

void ShowAppMenu(UINT menuId) {
    HWND owner = nullptr;
    for (size_t index = 0; index < g_app.menuButtons.size(); ++index) {
        if (menuId == ID_MENU_FILE + index) owner = g_app.menuButtons[index];
    }
    if (!owner) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    if (menuId == ID_MENU_FILE) {
        AppendMenuW(menu, MF_STRING, ID_NEW_TAB, L"New tab\tCtrl+T");
        AppendMenuW(menu, MF_STRING, ID_CLOSE_TAB, L"Close tab\tCtrl+W");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU newItems = BuildNewItemsMenu();
        AppendMenuW(menu, MF_POPUP | (g_app.panes[g_app.activePane].mode == PaneMode::Filesystem ? 0 : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(newItems), L"New");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Settings...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit\tAlt+F4");
    } else if (menuId == ID_MENU_EDIT) {
        AppendMenuW(menu, MF_STRING, ID_CLIP_COPY, L"Copy\tCtrl+C");
        AppendMenuW(menu, MF_STRING, ID_CLIP_CUT, L"Cut\tCtrl+X");
        AppendMenuW(menu, MF_STRING, ID_CLIP_PASTE, L"Paste\tCtrl+V");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_RENAME, L"Rename\tF2");
        AppendMenuW(menu, MF_STRING, ID_SELECT_ALL, L"Select all\tCtrl+A");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_DELETE, L"Move to Recycle Bin\tDelete");
        AppendMenuW(menu, MF_STRING, ID_DELETE_PERMANENT, L"Delete permanently\tShift+Delete");
    } else if (menuId == ID_MENU_VIEW) {
        AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh");
        AppendMenuW(menu, MF_STRING | (g_app.dualPane ? MF_CHECKED : 0), ID_DUAL, L"Dual pane");
        AppendMenuW(menu, MF_STRING | (g_app.preferences.sidebarVisible ? MF_CHECKED : 0), ID_PREF_SIDEBAR_VISIBLE, L"Navigation sidebar");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU layout = CreatePopupMenu();
        for (UINT viewId = ID_VIEW_EXTRA_LARGE; viewId <= ID_VIEW_CONTENT; ++viewId) {
            const UINT state = CommandFromViewMode(g_app.panes[g_app.activePane].view) == viewId ? MF_CHECKED : 0;
            AppendMenuW(layout, MF_STRING | state, viewId, ViewModeName(ViewModeFromCommand(viewId)));
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layout), L"Layout");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (g_app.theme == ThemeMode::Light ? MF_CHECKED : 0), ID_LIGHT, L"Light mode");
        AppendMenuW(menu, MF_STRING | (g_app.theme == ThemeMode::Dark ? MF_CHECKED : 0), ID_DARK, L"Dark mode");
    } else {
        AppendMenuW(menu, MF_STRING, ID_SHORTCUTS, L"Keyboard shortcuts");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_CHECK_UPDATES, L"Check for updates");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About Files4Me");
    }
    RECT rect{};
    GetWindowRect(owner, &rect);
    SetForegroundWindow(g_app.window);
    PrepareThemedMenu(menu);
    const UINT selected = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                               rect.left, rect.bottom, g_app.window);
    PostMessageW(g_app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
}

void ShowTabContextMenu(int paneIndex, POINT point) {
    if (paneIndex < 0 || paneIndex >= 2) return;
    Pane& pane = g_app.panes[paneIndex];
    POINT clientPoint = point;
    if (point.x == -1 && point.y == -1) {
        RECT itemRect{};
        if (TabCtrl_GetItemRect(pane.tab, pane.activeTab, &itemRect)) {
            clientPoint = {itemRect.left + Scale(12), itemRect.bottom};
            ClientToScreen(pane.tab, &clientPoint);
            point = clientPoint;
        }
    } else {
        ScreenToClient(pane.tab, &clientPoint);
        TCHITTESTINFO hit{};
        hit.pt = clientPoint;
        const int index = TabCtrl_HitTest(pane.tab, &hit);
        if (index >= 0 && index < static_cast<int>(pane.tabs.size()) && index != pane.activeTab) {
            pane.activeTab = index;
            TabCtrl_SetCurSel(pane.tab, index);
            SetActivePane(paneIndex);
            Navigate(paneIndex, pane.tabs[index].path, false);
            InvalidateRect(pane.tab, nullptr, TRUE);
        }
    }

    SetActivePane(paneIndex);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_NEW_TAB, L"New tab\tCtrl+T");
    AppendMenuW(menu, MF_STRING | (pane.tabs.size() <= 1 ? MF_GRAYED : 0), ID_CLOSE_TAB,
                L"Close tab\tCtrl+W");
    PrepareThemedMenu(menu);
    SetForegroundWindow(g_app.window);
    const UINT selected = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                               point.x, point.y, g_app.window);
    PostMessageW(g_app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
}

void PauseResumeRetryPrimaryJob() {
    auto job = PrimaryOperationJob(); if (!job) return;
    const JobState state = job->state;
    if (state == JobState::Running) job->pauseRequested = true;
    else if (state == JobState::Paused) {
        if (job->pauseRequested) { job->pauseRequested = false; job->controlChanged.notify_all(); }
        else job->state = JobState::Queued;
    } else if (state == JobState::Queued) job->state = JobState::Paused;
    else if (state == JobState::Failed || state == JobState::Interrupted) {
        job->cancelRequested = false; job->pauseRequested = false; job->result = S_OK;
        job->progressDone = 0; job->progressTotal = 0; job->state = JobState::Queued; PumpOperationQueue();
    }
    SaveJobJournal(); LayoutWindow();
}

void CancelPrimaryJob() {
    auto job = PrimaryOperationJob(); if (!job) return;
    if (job->state == JobState::Queued || (job->state == JobState::Paused && !job->pauseRequested)) {
        job->result = HRESULT_FROM_WIN32(ERROR_CANCELLED); job->state = JobState::Failed;
    } else {
        job->state = JobState::Cancelling; job->cancelRequested = true; job->pauseRequested = false; job->controlChanged.notify_all();
    }
    SaveJobJournal(); LayoutWindow();
}

void ClearCompletedJobs() {
    {
        std::lock_guard lock(g_app.operations.mutex);
        std::erase_if(g_app.operations.jobs, [](const auto& job) { return job->state == JobState::Completed; });
    }
    SaveJobJournal(); LayoutWindow();
}

void BeginPendingRename(Pane& pane) {
    if (pane.pendingRenamePath.empty()) return;
    const std::wstring target = std::move(pane.pendingRenamePath);
    pane.pendingRenamePath.clear();
    for (int row = 0; row < static_cast<int>(pane.items.size()); ++row) {
        if (!pane.items[row].IsActionable() || _wcsicmp(pane.items[row].fullPath.c_str(), target.c_str()) != 0) continue;
        ListView_SetItemState(pane.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(pane.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(pane.list, row, FALSE);
        SetFocus(pane.list);
        PostMessageW(pane.list, LVM_EDITLABELW, static_cast<WPARAM>(row), 0);
        return;
    }
}

void ExecuteCommand(UINT id) {
    Pane& active = g_app.panes[g_app.activePane];
    if (id >= ID_NEW_TEMPLATE_BASE && id <= ID_NEW_TEMPLATE_LAST) {
        CreateNewFromTemplate(id); return;
    }
    switch (id) {
    case ID_BACK: NavigateHistory(g_app.activePane, true); break;
    case ID_FORWARD: NavigateHistory(g_app.activePane, false); break;
    case ID_UP:
        if (active.mode == PaneMode::Filesystem) Navigate(g_app.activePane, ParentPath(active.tabs[active.activeTab].path));
        else if (active.mode == PaneMode::Archive) {
            if (active.archiveInternalPath.empty()) Navigate(g_app.activePane, ParentPath(active.archivePath));
            else {
                const size_t slash = active.archiveInternalPath.find_last_of(L"\\/");
                Navigate(g_app.activePane, ArchiveLocation(active.archivePath,
                         slash == std::wstring::npos ? L"" : active.archiveInternalPath.substr(0, slash)));
            }
        }
        break;
    case ID_REFRESH:
        if (active.mode == PaneMode::DriveOverview) StartDriveOverview(g_app.activePane);
        else if (active.mode == PaneMode::RecycleBin) LoadRecycleBin(g_app.activePane);
        else if (active.mode == PaneMode::Archive) StartArchiveEnumeration(g_app.activePane, active.archivePath, active.archiveInternalPath);
        else StartEnumeration(g_app.activePane, active.tabs[active.activeTab].path);
        break;
    case ID_NEW_FOLDER: if (active.mode == PaneMode::RecycleBin) EmptyRecycleBin(); else if (active.mode == PaneMode::Archive) ExtractAllArchive(); else if (active.mode == PaneMode::Filesystem) ShowNewMenu(); break;
    case ID_CREATE_FOLDER: CreateNewFromTemplate(ID_CREATE_FOLDER); break;
    case ID_CREATE_TEXT: CreateNewFromTemplate(ID_CREATE_TEXT); break;
    case ID_LIGHT: g_app.theme = ThemeMode::Light; ApplyTheme(); break;
    case ID_DARK: g_app.theme = ThemeMode::Dark; ApplyTheme(); break;
    case ID_THEME_TOGGLE: g_app.theme = g_app.theme == ThemeMode::Light ? ThemeMode::Dark : ThemeMode::Light; ApplyTheme(); break;
    case ID_VIEW_LAYOUT: ShowLayoutFlyout(); break;
    case ID_VIEW_EXTRA_LARGE: case ID_VIEW_LARGE: case ID_VIEW_MEDIUM: case ID_VIEW_SMALL:
    case ID_VIEW_LIST: case ID_VIEW_DETAILS: case ID_VIEW_TILES: case ID_VIEW_CONTENT:
        ApplyPaneView(g_app.activePane, ViewModeFromCommand(id)); break;
    case ID_SORT_NAME: SortPane(g_app.activePane, SortColumn::Name); break;
    case ID_SORT_EXTENSION: SortPane(g_app.activePane, SortColumn::Extension); break;
    case ID_SORT_SIZE: SortPane(g_app.activePane, SortColumn::Size); break;
    case ID_SORT_MODIFIED: SortPane(g_app.activePane, SortColumn::Modified); break;
    case ID_FOLDER_PROPERTIES: {
        if (active.mode != PaneMode::Filesystem || active.tabs.empty()) break;
        const std::wstring path = active.tabs[active.activeTab].path;
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_INVOKEIDLIST;
        execute.hwnd = g_app.window; execute.lpVerb = L"properties"; execute.lpFile = path.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execute)) ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Show folder properties");
        break;
    }
    case ID_PIN_SIDEBAR: {
        const auto paths = SelectedPaths(g_app.activePane);
        if (paths.size() != 1 || IsPinnedFolder(paths.front())) break;
        const DWORD attributes = GetFileAttributesW(ExtendedPath(paths.front()).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) break;
        g_app.pinnedFolders.push_back(paths.front());
        RebuildSidebar(); SaveSettings();
        break;
    }
    case ID_UNPIN_SIDEBAR:
        if (!g_app.pendingSidebarTarget.empty()) {
            std::erase_if(g_app.pinnedFolders, [&](const std::wstring& path) { return SamePath(path, g_app.pendingSidebarTarget); });
            g_app.pendingSidebarTarget.clear(); RebuildSidebar(); SaveSettings();
        }
        break;
    case ID_DUAL: g_app.dualPane = !g_app.dualPane; UpdateDualPaneButton(); LayoutWindow(); break;
    case ID_SWITCH_PANE:
        SetActivePane(1 - g_app.activePane); SetFocus(g_app.panes[g_app.activePane].list); LayoutWindow(); break;
    case ID_OPEN: OpenFocused(); break;
    case ID_COPY: case ID_MOVE: CopyMoveDelete(id); break;
    case ID_DELETE:
        if (active.mode == PaneMode::RecycleBin) InvokeRecycleVerb("delete", L"delete", false);
        else CopyMoveDelete(id);
        break;
    case ID_DELETE_PERMANENT:
        if (active.mode == PaneMode::RecycleBin) InvokeRecycleVerb("delete", L"delete", false);
        else CopyMoveDelete(id);
        break;
    case ID_RENAME: {
        const int index = ListView_GetNextItem(active.list, -1, LVNI_SELECTED);
        if (index >= 0 && ListView_GetNextItem(active.list, index, LVNI_SELECTED) == -1) {
            ListView_SetItemState(active.list, index, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(active.list, index, FALSE);
            SetFocus(active.list);
            PostMessageW(active.list, LVM_EDITLABELW, static_cast<WPARAM>(index), 0);
        }
        break;
    }
    case ID_NEW_TAB: AddTab(g_app.activePane); break;
    case ID_CLOSE_TAB: CloseTab(g_app.activePane); break;
    case ID_FOCUS_PATH: SetFocus(active.path); SendMessageW(active.path, EM_SETSEL, 0, -1); break;
    case ID_FOCUS_SEARCH: SetFocus(active.search); SendMessageW(active.search, EM_SETSEL, 0, -1); break;
    case ID_SELECT_ALL: ListView_SetItemState(active.list, -1, LVIS_SELECTED, LVIS_SELECTED); break;
    case ID_CLIP_COPY: PutSelectionOnClipboard(false); break;
    case ID_CLIP_CUT: if (active.mode == PaneMode::RecycleBin) InvokeRecycleVerb("undelete", L"undelete", false); else PutSelectionOnClipboard(true); break;
    case ID_CLIP_PASTE: PasteClipboard(); break;
    case ID_MORE: ShowSelectionMoreMenu(); break;
    case ID_EXTRACT_ALL:
        if (active.mode == PaneMode::Archive) ExtractAllArchive();
        else ExtractZipFile(SelectedZipPath(g_app.activePane));
        break;
    case ID_RECYCLE_RESTORE: InvokeRecycleVerb("undelete", L"undelete", false); break;
    case ID_RECYCLE_RESTORE_ALL: InvokeRecycleVerb("undelete", L"undelete", true); break;
    case ID_RECYCLE_EMPTY: EmptyRecycleBin(); break;
    case ID_OPEN_WITH: InvokeSelectionShellVerb("openas", L"openas"); break;
    case ID_COPY_PATH: CopySelectedPathsAsText(); break;
    case ID_PROPERTIES: InvokeSelectionShellVerb("properties", L"properties"); break;
    case ID_SHELL_CONTEXT: {
        RECT rect{}; GetWindowRect(g_app.selectionButtons[5], &rect);
        ShowContextMenu(g_app.activePane, {rect.right, rect.bottom});
        break;
    }
    case ID_SETTINGS: ShowSettingsWindow(); break;
    case ID_CHECK_UPDATES: StartUpdateCheck(true); break;
    case ID_JOB_PAUSE: PauseResumeRetryPrimaryJob(); break;
    case ID_JOB_CANCEL: CancelPrimaryJob(); break;
    case ID_JOB_CLEAR: ClearCompletedJobs(); break;
    case ID_PREF_SIDEBAR_VISIBLE: ApplyPreferenceCommand(ID_PREF_SIDEBAR_VISIBLE); break;
    case ID_MENU_FILE: case ID_MENU_EDIT: case ID_MENU_VIEW: case ID_MENU_HELP: ShowAppMenu(id); break;
    case ID_EXIT: SendMessageW(g_app.window, WM_CLOSE, 0, 0); break;
    case ID_ABOUT:
        MessageBoxW(g_app.window,
                    L"Files4Me " FILES4ME_VERSION_DISPLAY_W L"\n\nLightweight native dual-pane file manager.\nC++20 and Windows system APIs only.\n\nMaterial Icons by Google, Apache License 2.0.",
                    L"About Files4Me", MB_OK | MB_ICONINFORMATION);
        break;
    case ID_SHORTCUTS:
        MessageBoxW(g_app.window,
                    L"Tab  Switch pane\nEnter  Open\nF2  Rename\nF5  Copy to other pane\nF6  Move to other pane\nF7  New folder\nF8/Delete  Recycle\nCtrl+T/W  New/close tab\nCtrl+C/X/V  Copy/cut/paste\nAlt+Left/Right/Up  Navigate",
                    L"Keyboard shortcuts", MB_OK | MB_ICONINFORMATION);
        break;
    }
}

LRESULT HandleNotify(NMHDR* header) {
    if (header->idFrom == ID_SIDEBAR) {
        if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
            const int item = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
            if (item >= 0 && item < static_cast<int>(g_app.sidebarItems.size()) &&
                g_app.sidebarItems[item].action != SidebarAction::Separator) ActivateSidebarItem(item);
        } else if (header->code == NM_RCLICK) {
            const int item = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
            if (item >= 0 && item < static_cast<int>(g_app.sidebarItems.size()) && g_app.sidebarItems[item].pinned) {
                POINT point{}; GetCursorPos(&point);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_UNPIN_SIDEBAR, L"Unpin from sidebar");
                PrepareThemedMenu(menu);
                g_app.pendingSidebarTarget = g_app.sidebarItems[item].target;
                const UINT selected = TrackModernPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, g_app.window);
                DestroyMenu(menu);
                if (selected) SendMessageW(g_app.window, WM_COMMAND, selected, 0);
                else g_app.pendingSidebarTarget.clear();
            }
        } else if (header->code == NM_CUSTOMDRAW) {
            auto* custom = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
            if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const int item = static_cast<int>(custom->nmcd.dwItemSpec);
                if (item >= 0 && item < static_cast<int>(g_app.sidebarItems.size()) &&
                    g_app.sidebarItems[item].action == SidebarAction::Separator) {
                    RECT row{};
                    if (!ListView_GetItemRect(g_app.sidebar, item, &row, LVIR_BOUNDS)) row = custom->nmcd.rc;
                    FillRect(custom->nmcd.hdc, &row, g_app.windowBrush);
                    const int inset = Scale(8);
                    const int dividerY = row.top + (row.bottom - row.top) / 2;
                    RECT divider{row.left + inset, dividerY, row.right - inset, dividerY + Scale(1)};
                    HBRUSH dividerBrush = CreateSolidBrush(g_app.theme == ThemeMode::Dark
                        ? RGB(58, 58, 58) : RGB(210, 210, 210));
                    FillRect(custom->nmcd.hdc, &divider, dividerBrush);
                    DeleteObject(dividerBrush);
                    return CDRF_SKIPDEFAULT;
                } else {
                    const bool selected = (ListView_GetItemState(g_app.sidebar, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                    custom->clrText = selected ? g_app.colors.selectionText : g_app.colors.text;
                    custom->clrTextBk = selected ? g_app.colors.selection : g_app.colors.window;
                }
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }
    int paneIndex = PaneForId(static_cast<UINT>(header->idFrom));
    if (paneIndex < 0) return 0;
    Pane& pane = g_app.panes[paneIndex];
    const UINT local = LocalId(static_cast<UINT>(header->idFrom));
    if (local == ID_TAB && header->code == TCN_SELCHANGE) {
        SetActivePane(paneIndex);
        pane.activeTab = TabCtrl_GetCurSel(pane.tab);
        Navigate(paneIndex, pane.tabs[pane.activeTab].path, false);
    } else if (local == ID_LIST && header->code == LVN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
        if (pane.mode == PaneMode::DriveOverview) {
            if (info->item.iItem < 0 || info->item.iItem >= static_cast<int>(pane.driveItems.size())) return 0;
            const DriveItem& drive = pane.driveItems[info->item.iItem];
            if (info->item.mask & LVIF_TEXT) {
                const std::wstring text = drive.label + L" (" + drive.root.substr(0, 2) + L")";
                StringCchCopyW(info->item.pszText, info->item.cchTextMax, text.c_str());
            }
            if (info->item.mask & LVIF_IMAGE) info->item.iImage = drive.icon;
            return 0;
        }
        if (pane.mode == PaneMode::RecycleBin) {
            if (info->item.iItem < 0 || info->item.iItem >= static_cast<int>(pane.recycleItems.size())) return 0;
            const RecycleItem& item = pane.recycleItems[info->item.iItem];
            if (info->item.mask & LVIF_TEXT) {
                std::wstring text;
                if (info->item.iSubItem == 0) text = item.name;
                else if (info->item.iSubItem == 1) text = item.originalLocation;
                else if (info->item.iSubItem == 2) text = FormatTime(item.deleted);
                else text = item.size ? FormatSize(item.size) : L"";
                StringCchCopyW(info->item.pszText, info->item.cchTextMax, text.c_str());
            }
            if (info->item.mask & LVIF_IMAGE) info->item.iImage = CachedSmallIconIndex(item.icon);
            return 0;
        }
        if (info->item.iItem < 0 || info->item.iItem >= static_cast<int>(pane.items.size())) return 0;
        const FileItem& item = pane.items[info->item.iItem];
        if (item.timelineHeader) {
            if (info->item.mask & LVIF_TEXT)
                StringCchCopyW(info->item.pszText, info->item.cchTextMax, item.name.c_str());
            if (info->item.mask & LVIF_IMAGE) info->item.iImage = I_IMAGENONE;
            return 0;
        }
        if (info->item.mask & LVIF_TEXT) {
            std::wstring text;
            if (info->item.iSubItem == 0) text = item.name;
            else if (info->item.iSubItem == 1) text = g_app.preferences.showExtensions ? item.extension : L"";
            else if (info->item.iSubItem == 2) text = item.IsDirectory() ? L"" : FormatSize(item.size);
            else text = FormatTime(item.modified);
            StringCchCopyW(info->item.pszText, info->item.cchTextMax, text.c_str());
        }
        if (info->item.mask & LVIF_IMAGE) {
            if (pane.view <= FileViewMode::MediumIcons)
                info->item.iImage = CachedIconIndex(item.icon, static_cast<size_t>(pane.view), item.IsDirectory());
            else if (pane.view == FileViewMode::Content)
                info->item.iImage = CachedIconIndex(item.icon, 1, item.IsDirectory());
            else if (pane.view == FileViewMode::Tiles)
                info->item.iImage = CachedIconIndex(item.icon, 2, item.IsDirectory());
            else info->item.iImage = CachedSmallIconIndex(item.icon, item.IsDirectory());
        }
    } else if (local == ID_LIST && header->code == NM_DBLCLK) {
        SetActivePane(paneIndex);
        const int row = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
        if (row >= 0 && row < static_cast<int>(pane.items.size()) && pane.items[row].timelineHeader) {
            const int bucket = pane.items[row].timelineBucket;
            pane.collapsedTimelineGroups[bucket] = !pane.collapsedTimelineGroups[bucket];
            RebuildTimelineRows(pane);
        } else if (row >= 0) OpenFocused();
    } else if (local == ID_LIST && header->code == NM_RCLICK) {
        SetActivePane(paneIndex);
        const int row = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
        POINT point{}; GetCursorPos(&point);
        if (row < 0) ShowBackgroundContextMenu(paneIndex, point);
        else ShowContextMenu(paneIndex, point);
    } else if (local == ID_LIST && header->code == LVN_BEGINDRAG) {
        SetActivePane(paneIndex);
        const int item = reinterpret_cast<NMLISTVIEW*>(header)->iItem;
        if (item >= 0 && item < static_cast<int>(pane.items.size()) && pane.items[item].IsActionable()) {
            if ((ListView_GetItemState(pane.list, item, LVIS_SELECTED) & LVIS_SELECTED) == 0)
                ListView_SetItemState(pane.list, item, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
            StartShellDrag(paneIndex);
        }
    } else if (local == ID_LIST && header->code == LVN_ITEMCHANGED) {
        auto* change = reinterpret_cast<NMLISTVIEW*>(header);
        if ((change->uChanged & LVIF_STATE) != 0 &&
            ((change->uOldState ^ change->uNewState) & LVIS_SELECTED) != 0) {
            InvalidateListItem(pane.list, change->iItem);
            if (paneIndex == g_app.activePane) UpdateSelectionCommands();
        }
    } else if (local == ID_LIST && header->code == LVN_ODSTATECHANGED) {
        InvalidateRect(pane.list, nullptr, FALSE);
        if (paneIndex == g_app.activePane) UpdateSelectionCommands();
    } else if (local == ID_LIST && header->code == NM_CLICK) {
        SetActivePane(paneIndex);
        const int row = reinterpret_cast<NMITEMACTIVATE*>(header)->iItem;
        if (row >= 0 && row < static_cast<int>(pane.items.size()) && pane.items[row].timelineHeader) {
            const int bucket = pane.items[row].timelineBucket;
            pane.collapsedTimelineGroups[bucket] = !pane.collapsedTimelineGroups[bucket];
            RebuildTimelineRows(pane);
        }
    } else if (local == ID_LIST && header->code == LVN_ITEMCHANGING) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
        if (change->iItem >= 0 && change->iItem < static_cast<int>(pane.items.size()) &&
            pane.items[change->iItem].timelineHeader &&
            ((change->uNewState & (LVIS_SELECTED | LVIS_FOCUSED)) != 0)) return TRUE;
    } else if (local == ID_LIST && header->code == NM_SETFOCUS) {
        SetActivePane(paneIndex);
    } else if (local == ID_LIST && header->code == LVN_BEGINLABELEDITW) {
        SetActivePane(paneIndex);
        const auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
        if (info->item.iItem >= 0 && info->item.iItem < static_cast<int>(pane.items.size()) &&
            pane.items[info->item.iItem].IsActionable()) {
            const FileItem& item = pane.items[info->item.iItem];
            size_t selectionEnd = item.name.size();
            if (!item.IsDirectory()) {
                const size_t dot = item.name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot != 0) selectionEnd = dot;
            }
            if (HWND edit = ListView_GetEditControl(pane.list))
                PostMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(selectionEnd));
        }
        return FALSE;
    } else if (local == ID_LIST && header->code == LVN_ENDLABELEDITW) {
        SetActivePane(paneIndex);
        const auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
        if (info->item.pszText) RenameItem(paneIndex, info->item.iItem, info->item.pszText);
        return FALSE;
    } else if (local == ID_LIST && header->code == NM_CUSTOMDRAW) {
        auto* custom = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
        if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (custom->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const int itemIndex = static_cast<int>(custom->nmcd.dwItemSpec);
            if (custom->iSubItem == 0 && itemIndex >= 0 && itemIndex < static_cast<int>(pane.items.size()) &&
                pane.items[itemIndex].IsActionable() && pane.view == FileViewMode::Details) {
                const FileItem& item = pane.items[itemIndex];
                RECT row{};
                if (!ListView_GetItemRect(pane.list, itemIndex, &row, LVIR_BOUNDS)) row = custom->nmcd.rc;
                RECT cell{row.left, row.top,
                          (std::min)(row.right, row.left + ListView_GetColumnWidth(pane.list, 0)), row.bottom};
                const int savedDc = SaveDC(custom->nmcd.hdc);
                IntersectClipRect(custom->nmcd.hdc, cell.left, cell.top, cell.right, cell.bottom);
                const bool selected = (ListView_GetItemState(pane.list, itemIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                const bool hovered = pane.hoveredCheckboxItem == itemIndex;
                HBRUSH background = CreateSolidBrush(selected ? g_app.colors.selection :
                                                     (hovered ? g_app.colors.buttonPressed : g_app.colors.surface));
                FillRect(custom->nmcd.hdc, &cell, background); DeleteObject(background);
                const int checkboxSize = (std::max)(Scale(18), 16);
                const bool showCheckbox = g_app.checkboxImages && (selected || pane.hoveredCheckboxItem == itemIndex);
                if (showCheckbox) ImageList_Draw(g_app.checkboxImages, selected ? 2 : 1, custom->nmcd.hdc,
                    cell.left + Scale(4), cell.top + ((cell.bottom - cell.top) - checkboxSize) / 2, ILD_TRANSPARENT);
                const int iconX = cell.left + Scale(4) + checkboxSize + Scale(8);
                const int iconY = cell.top + ((cell.bottom - cell.top) - g_app.smallIconCache.pixels) / 2;
                const int image = CachedSmallIconIndex(item.icon, item.IsDirectory());
                ImageList_Draw(g_app.smallIconCache.images, image, custom->nmcd.hdc, iconX, iconY,
                               ILD_TRANSPARENT | (IsCutPath(item.fullPath) ? ILD_BLEND50 : 0));
                RECT text{iconX + g_app.smallIconCache.pixels + Scale(7), cell.top,
                          cell.right - Scale(5), cell.bottom};
                SetBkMode(custom->nmcd.hdc, TRANSPARENT);
                SetTextColor(custom->nmcd.hdc, IsCutPath(item.fullPath) ? g_app.colors.muted :
                             (selected ? g_app.colors.selectionText : g_app.colors.text));
                SelectObject(custom->nmcd.hdc, g_app.fileFont ? g_app.fileFont : g_app.font);
                DrawTextW(custom->nmcd.hdc, item.name.c_str(), -1, &text,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                RestoreDC(custom->nmcd.hdc, savedDc);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const int itemIndex = static_cast<int>(custom->nmcd.dwItemSpec);
            const bool selected = (ListView_GetItemState(pane.list, itemIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            const bool hovered = pane.hoveredCheckboxItem == itemIndex;
            if (itemIndex >= 0 && itemIndex < static_cast<int>(pane.items.size()) && pane.items[itemIndex].timelineHeader) {
                RECT row{};
                if (!ListView_GetItemRect(pane.list, itemIndex, &row, LVIR_BOUNDS)) row = custom->nmcd.rc;
                FillRect(custom->nmcd.hdc, &row, g_app.surfaceBrush);
                const FileItem& timeline = pane.items[itemIndex];
                const int chevronX = row.left + Scale(13), centerY = (row.top + row.bottom) / 2;
                HPEN pen = CreatePen(PS_SOLID, Scale(1), g_app.colors.muted);
                HGDIOBJ oldPen = SelectObject(custom->nmcd.hdc, pen);
                if (pane.collapsedTimelineGroups[timeline.timelineBucket]) {
                    MoveToEx(custom->nmcd.hdc, chevronX - Scale(2), centerY - Scale(4), nullptr);
                    LineTo(custom->nmcd.hdc, chevronX + Scale(2), centerY);
                    LineTo(custom->nmcd.hdc, chevronX - Scale(2), centerY + Scale(4));
                } else {
                    MoveToEx(custom->nmcd.hdc, chevronX - Scale(4), centerY - Scale(2), nullptr);
                    LineTo(custom->nmcd.hdc, chevronX, centerY + Scale(2));
                    LineTo(custom->nmcd.hdc, chevronX + Scale(4), centerY - Scale(2));
                }
                SelectObject(custom->nmcd.hdc, oldPen); DeleteObject(pen);
                RECT title{row.left + Scale(27), row.top, row.right - Scale(70), row.bottom};
                SetBkMode(custom->nmcd.hdc, TRANSPARENT); SetTextColor(custom->nmcd.hdc, g_app.colors.text);
                SelectObject(custom->nmcd.hdc, g_app.fontBold);
                DrawTextW(custom->nmcd.hdc, timeline.name.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                const std::wstring count = std::to_wstring(timeline.size) + L" items";
                RECT countRect{row.right - Scale(68), row.top, row.right - Scale(10), row.bottom};
                SelectObject(custom->nmcd.hdc, g_app.font); SetTextColor(custom->nmcd.hdc, g_app.colors.muted);
                DrawTextW(custom->nmcd.hdc, count.c_str(), -1, &countRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                RECT divider{row.left + Scale(8), row.bottom - 1, row.right - Scale(8), row.bottom};
                HBRUSH dividerBrush = CreateSolidBrush(g_app.colors.border); FillRect(custom->nmcd.hdc, &divider, dividerBrush); DeleteObject(dividerBrush);
                return CDRF_SKIPDEFAULT;
            }
            if (pane.mode == PaneMode::DriveOverview && itemIndex >= 0 && itemIndex < static_cast<int>(pane.driveItems.size())) {
                const DriveItem& drive = pane.driveItems[itemIndex];
                RECT row{};
                if (!ListView_GetItemRect(pane.list, itemIndex, &row, LVIR_BOUNDS)) row = custom->nmcd.rc;
                HBRUSH background = CreateSolidBrush(selected ? g_app.colors.selection :
                                                     (hovered ? g_app.colors.buttonPressed : g_app.colors.surface));
                FillRect(custom->nmcd.hdc, &row, background); DeleteObject(background);
                const int iconSize = g_app.iconCaches[2].pixels;
                const int iconIndex = CachedIconIndex(drive.icon, 2);
                ImageList_Draw(g_app.iconCaches[2].images, iconIndex, custom->nmcd.hdc,
                               row.left + Scale(8), row.top + Scale(10), ILD_TRANSPARENT);
                const int textLeft = row.left + Scale(15) + iconSize;
                const int contentRight = (std::min)(static_cast<int>(row.right) - Scale(8), textLeft + Scale(170));
                std::wstring title = drive.label + L" (" + drive.root.substr(0, 2) + L")";
                RECT titleRect{textLeft, row.top + Scale(5), contentRight, row.top + Scale(25)};
                SetBkMode(custom->nmcd.hdc, TRANSPARENT); SelectObject(custom->nmcd.hdc, g_app.fontBold);
                SetTextColor(custom->nmcd.hdc, selected ? g_app.colors.selectionText : g_app.colors.text);
                DrawTextW(custom->nmcd.hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (drive.available && drive.total) {
                    const double freeRatio = static_cast<double>(drive.free) / static_cast<double>(drive.total);
                    const COLORREF capacity = freeRatio < .10 ? RGB(210,55,55) : freeRatio < .20 ? RGB(225,170,0) :
                                              freeRatio < .50 ? RGB(0,120,215) : RGB(46,160,67);
                    RECT track{textLeft, row.top + Scale(23), contentRight, row.top + Scale(36)};
                    HRGN trackRegion = CreateRoundRectRgn(track.left, track.top, track.right + 1, track.bottom + 1,
                                                          Scale(4), Scale(4));
                    HBRUSH trackBrush = CreateSolidBrush(g_app.colors.button);
                    FillRgn(custom->nmcd.hdc, trackRegion, trackBrush); DeleteObject(trackBrush);
                    RECT used = track; used.right = used.left + static_cast<int>((used.right - used.left) * (1.0 - freeRatio));
                    const int savedDc = SaveDC(custom->nmcd.hdc);
                    SelectClipRgn(custom->nmcd.hdc, trackRegion);
                    HBRUSH usedBrush = CreateSolidBrush(capacity); FillRect(custom->nmcd.hdc, &used, usedBrush); DeleteObject(usedBrush);
                    RestoreDC(custom->nmcd.hdc, savedDc); DeleteObject(trackRegion);
                    const std::wstring space = FormatSize(drive.free) + L" free of " + FormatSize(drive.total);
                    RECT detail{textLeft, row.top + Scale(40), contentRight, row.bottom - Scale(3)};
                    SelectObject(custom->nmcd.hdc, g_app.font);
                    SetTextColor(custom->nmcd.hdc, selected ? g_app.colors.selectionText : g_app.colors.muted);
                    DrawTextW(custom->nmcd.hdc, space.c_str(), -1, &detail, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                } else {
                    RECT detail{textLeft, row.top + Scale(37), contentRight, row.bottom - Scale(4)};
                    SetTextColor(custom->nmcd.hdc, selected ? g_app.colors.selectionText : g_app.colors.muted);
                    DrawTextW(custom->nmcd.hdc, L"Drive not ready", -1, &detail, DT_LEFT | DT_SINGLELINE);
                }
                return CDRF_SKIPDEFAULT;
            }
            const bool cut = itemIndex >= 0 && itemIndex < static_cast<int>(pane.items.size()) &&
                             IsCutPath(pane.items[itemIndex].fullPath);
            if (pane.view == FileViewMode::Content && itemIndex >= 0 && itemIndex < static_cast<int>(pane.items.size())) {
                const FileItem& item = pane.items[itemIndex];
                RECT row = custom->nmcd.rc;
                HBRUSH background = CreateSolidBrush(selected ? g_app.colors.selection :
                                                     (hovered ? g_app.colors.buttonPressed : g_app.colors.surface));
                FillRect(custom->nmcd.hdc, &row, background); DeleteObject(background);
                const int checkboxSize = (std::max)(Scale(18), 16);
                if (g_app.checkboxImages && (selected || pane.hoveredCheckboxItem == itemIndex)) {
                    const int checkboxY = row.top + ((row.bottom - row.top) - checkboxSize) / 2;
                    ImageList_Draw(g_app.checkboxImages, selected ? 2 : 1, custom->nmcd.hdc,
                                   row.left + Scale(8), checkboxY, ILD_TRANSPARENT);
                }
                const int imageIndex = CachedIconIndex(item.icon, 1, item.IsDirectory());
                const int imageY = row.top + (static_cast<int>(row.bottom - row.top) - g_app.iconCaches[1].pixels) / 2;
                const int imageX = row.left + Scale(8) + checkboxSize + Scale(8);
                ImageList_Draw(g_app.iconCaches[1].images, imageIndex, custom->nmcd.hdc, imageX, imageY,
                               ILD_TRANSPARENT | (cut ? ILD_BLEND50 : 0));
                const int textLeft = imageX + g_app.iconCaches[1].pixels + Scale(12);
                RECT nameRect{textLeft, row.top + Scale(8), row.right - Scale(10), row.top + Scale(31)};
                SetBkMode(custom->nmcd.hdc, TRANSPARENT);
                SetTextColor(custom->nmcd.hdc, cut ? g_app.colors.muted :
                             (selected ? g_app.colors.selectionText : g_app.colors.text));
                SelectObject(custom->nmcd.hdc, g_app.fileFont ? g_app.fileFont : g_app.font);
                DrawTextW(custom->nmcd.hdc, item.name.c_str(), -1, &nameRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                std::wstring metadata = item.IsDirectory() ? L"File folder" : item.type;
                if (!item.IsDirectory()) metadata += L"  ·  " + FormatSize(item.size);
                metadata += L"  ·  " + FormatTime(item.modified);
                RECT detailRect{textLeft, row.top + Scale(33), row.right - Scale(10), row.bottom - Scale(5)};
                SetTextColor(custom->nmcd.hdc, cut ? g_app.colors.muted :
                             (selected ? g_app.colors.selectionText : g_app.colors.muted));
                DrawTextW(custom->nmcd.hdc, metadata.c_str(), -1, &detailRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                return CDRF_SKIPDEFAULT;
            }
            custom->clrText = cut ? g_app.colors.muted : (selected ? g_app.colors.selectionText : g_app.colors.text);
            custom->clrTextBk = selected ? g_app.colors.selection :
                                (hovered ? g_app.colors.buttonPressed : g_app.colors.surface);
            const bool customDetails = pane.view == FileViewMode::Details &&
                                       (pane.mode == PaneMode::Filesystem || pane.mode == PaneMode::Archive);
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT | (customDetails ? CDRF_NOTIFYSUBITEMDRAW : 0);
        }
        if (custom->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
            const int itemIndex = static_cast<int>(custom->nmcd.dwItemSpec);
            const bool detailsCheckboxDrawn = pane.view == FileViewMode::Details &&
                                              (pane.mode == PaneMode::Filesystem || pane.mode == PaneMode::Archive);
            if (!detailsCheckboxDrawn && PaneSupportsCheckboxes(pane) && g_app.checkboxImages && itemIndex >= 0) {
                const bool selected = (ListView_GetItemState(pane.list, itemIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                if (selected || pane.hoveredCheckboxItem == itemIndex) {
                    RECT row{};
                    if (ListView_GetItemRect(pane.list, itemIndex, &row, LVIR_BOUNDS)) {
                        const int checkboxSize = (std::max)(Scale(18), 16);
                        const bool iconLayout = pane.mode == PaneMode::Filesystem && pane.view <= FileViewMode::SmallIcons;
                        const int checkboxY = iconLayout ? row.top + Scale(3)
                                                         : row.top + ((row.bottom - row.top) - checkboxSize) / 2;
                        ImageList_Draw(g_app.checkboxImages, selected ? 2 : 1, custom->nmcd.hdc,
                                       row.left + Scale(3), checkboxY, ILD_TRANSPARENT);
                    }
                }
            }
            return CDRF_DODEFAULT;
        }
    }
    return 0;
}

void CreateSidebar(HWND parent) {
    g_app.sidebar = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(ID_SIDEBAR), g_app.instance, nullptr);
    ApplyFont(g_app.sidebar);
    ListView_SetExtendedListViewStyle(g_app.sidebar, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetImageList(g_app.sidebar, g_app.sidebarImages, LVSIL_SMALL);
    SetWindowSubclass(g_app.sidebar, SidebarScrollSubclassProc, 5, 0);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = Scale(190);
    ListView_InsertColumn(g_app.sidebar, 0, &column);
    RebuildSidebar();
}

void CreateUi(HWND window) {
    g_app.window = window;
    g_app.dpi = GetDpiForWindow(window);
    LoadMaterialIconFont();
    RebuildFonts();
    SHFILEINFOW shellInfo{};
    g_app.systemImages = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &shellInfo,
        sizeof(shellInfo), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    g_app.systemImagesLarge = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &shellInfo,
        sizeof(shellInfo), SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES));
    SHFILEINFOW genericInfo{};
    if (SHGetFileInfoW(L"Files4Me.__generic_file__", FILE_ATTRIBUTE_NORMAL, &genericInfo, sizeof(genericInfo),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
        g_app.genericFileIcon = genericInfo.iIcon;
    RebuildIconCaches();
    RebuildSidebarIconCache();

    const UINT ids[] = {ID_BACK, ID_FORWARD, ID_UP, ID_REFRESH, ID_NEW_FOLDER, ID_THEME_TOGGLE, ID_VIEW_LAYOUT, ID_DUAL};
    static const wchar_t* labels[] = {L"Back", L"Forward", L"Up", L"Refresh", L"New", L"Toggle light/dark theme", L"Change file layout", L"Dual pane"};
    for (size_t i = 0; i < g_app.commandButtons.size(); ++i) g_app.commandButtons[i] = CreateOwnerButton(window, ids[i], labels[i]);
    for (HWND button : g_app.commandButtons) SetWindowSubclass(button, ToolbarButtonSubclassProc, 1, 0);
    const UINT selectionIds[] = {ID_CLIP_CUT, ID_CLIP_COPY, ID_CLIP_PASTE, ID_RENAME, ID_DELETE, ID_MORE, ID_EXTRACT_ALL};
    static const wchar_t* selectionLabels[] = {L"Cut", L"Copy", L"Paste", L"Rename", L"Move to Recycle Bin", L"More", L"Extract all"};
    for (size_t i = 0; i < g_app.selectionButtons.size(); ++i) {
        g_app.selectionButtons[i] = CreateOwnerButton(window, selectionIds[i], selectionLabels[i]);
        SetWindowSubclass(g_app.selectionButtons[i], ToolbarButtonSubclassProc, 1, 0);
    }
    UpdateDualPaneButton();
    const UINT menuIds[] = {ID_MENU_FILE, ID_MENU_EDIT, ID_MENU_VIEW, ID_MENU_HELP};
    const wchar_t* menuLabels[] = {L"&File", L"&Edit", L"&View", L"&Help"};
    for (size_t i = 0; i < g_app.menuButtons.size(); ++i) g_app.menuButtons[i] = CreateOwnerButton(window, menuIds[i], menuLabels[i]);
    const UINT operationIds[] = {ID_JOB_PAUSE, ID_JOB_CANCEL, ID_JOB_CLEAR};
    const wchar_t* operationLabels[] = {L"Pause", L"Cancel", L"Clear"};
    for (size_t i = 0; i < g_app.operationButtons.size(); ++i) {
        g_app.operationButtons[i] = CreateOwnerButton(window, operationIds[i], operationLabels[i]);
        ShowWindow(g_app.operationButtons[i], SW_HIDE);
    }
    g_app.tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     window, nullptr, g_app.instance, nullptr);
    if (g_app.tooltips) {
        for (size_t i = 0; i < g_app.commandButtons.size(); ++i) {
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window;
            tool.uId = reinterpret_cast<UINT_PTR>(g_app.commandButtons[i]);
            tool.lpszText = const_cast<wchar_t*>(labels[i]);
            SendMessageW(g_app.tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        for (size_t i = 0; i < g_app.selectionButtons.size(); ++i) {
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window;
            tool.uId = reinterpret_cast<UINT_PTR>(g_app.selectionButtons[i]);
            tool.lpszText = const_cast<wchar_t*>(selectionLabels[i]);
            SendMessageW(g_app.tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    CreateSidebar(window);
    CreatePane(0); CreatePane(1);
    ApplyPaneView(0, g_app.panes[0].view); ApplyPaneView(1, g_app.panes[1].view);
    AddClipboardFormatListener(window);
    SyncCutVisualsFromClipboard();
    UpdateSelectionCommands();
    ApplyTheme(); LayoutWindow();
    Navigate(0, g_app.panes[0].tabs[0].path, false);
    Navigate(1, g_app.panes[1].tabs[0].path, false);
    SetTimer(window, 1, g_app.preferences.refreshMilliseconds, nullptr);
    PumpOperationQueue();
    if (g_app.preferences.automaticUpdates) StartUpdateCheck(false);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITMENUPOPUP: case WM_MEASUREITEM: case WM_MENUCHAR:
        if (g_app.activeContextMenu3) {
            LRESULT menuResult = 0;
            if (SUCCEEDED(g_app.activeContextMenu3->HandleMenuMsg2(message, wParam, lParam, &menuResult))) return menuResult;
        } else if (g_app.activeContextMenu2 &&
                   SUCCEEDED(g_app.activeContextMenu2->HandleMenuMsg(message, wParam, lParam))) return 0;
        if (message == WM_MEASUREITEM && MeasureThemedMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_CREATE: CreateUi(window); return 0;
    case WM_SIZE: LayoutWindow(); return 0;
    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RebuildFonts();
        RebuildIconCaches();
        RebuildSidebarIconCache();
        RebuildCheckboxImages();
        for (HWND button : g_app.commandButtons) ApplyFont(button);
        for (HWND button : g_app.selectionButtons) ApplyFont(button);
        for (HWND button : g_app.menuButtons) ApplyFont(button);
        for (HWND button : g_app.operationButtons) ApplyFont(button);
        for (Pane& pane : g_app.panes) {
            ApplyFont(pane.tab); ApplyFont(pane.drives); ApplyFont(pane.path); ApplyFont(pane.search); ApplyFont(pane.status);
            SendMessageW(pane.list, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fileFont), TRUE);
            for (HWND header : pane.headers) ApplyFont(header);
            TabCtrl_SetItemSize(pane.tab, Scale(150), Scale(29));
        }
        for (int paneIndex = 0; paneIndex < 2; ++paneIndex) ApplyPaneView(paneIndex, g_app.panes[paneIndex].view);
        for (Pane& pane : g_app.panes)
            if (pane.mode == PaneMode::RecycleBin) ListView_SetImageList(pane.list, g_app.smallIconCache.images, LVSIL_SMALL);
        LayoutWindow(); return 0;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: ApplyTheme(); return 0;
    case WM_CLIPBOARDUPDATE: SyncCutVisualsFromClipboard(); UpdateSelectionCommands(); return 0;
    case WM_TIMER:
        if (wParam == 1 && g_app.preferences.autoRefresh) {
            for (int index = 0; index < 2; ++index) {
                Pane& pane = g_app.panes[index];
                if (pane.enumerating || pane.tabs.empty() || pane.mode != PaneMode::Filesystem) continue;
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                const std::wstring path = pane.tabs[pane.activeTab].path;
                if (GetFileAttributesExW(ExtendedPath(path).c_str(), GetFileExInfoStandard, &attributes) &&
                    CompareFileTime(&attributes.ftLastWriteTime, &pane.directoryStamp) != 0) {
                    StartEnumeration(index, path);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        RECT client{}; GetClientRect(window, &client);
        const int drawerHeight = OperationDrawerHeight();
        if (drawerHeight && GET_Y_LPARAM(lParam) >= client.bottom - Scale(30) - drawerHeight) {
            g_app.operations.expanded = !g_app.operations.expanded; LayoutWindow(); return 0;
        }
        break;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        const int paneIndex = PaneForId(id);
        if (paneIndex >= 0) {
            switch (LocalId(id)) {
            case ID_DRIVES:
                if (HIWORD(wParam) == BN_CLICKED) { SetActivePane(paneIndex); ShowDriveMenu(paneIndex, reinterpret_cast<HWND>(lParam)); }
                break;
            case ID_PATH:
                if (HIWORD(wParam) == EN_SETFOCUS) SetActivePane(paneIndex);
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                    const std::wstring& current = g_app.panes[paneIndex].tabs[g_app.panes[paneIndex].activeTab].path;
                    SetWindowTextW(g_app.panes[paneIndex].path, (IsVirtualLocation(current) ? DisplayNameForPath(current) : current).c_str());
                }
                break;
            case ID_SEARCH:
                if (HIWORD(wParam) == EN_SETFOCUS) SetActivePane(paneIndex);
                if (HIWORD(wParam) == EN_CHANGE) {
                    wchar_t filter[512]{};
                    GetWindowTextW(g_app.panes[paneIndex].search, filter, ARRAYSIZE(filter));
                    g_app.panes[paneIndex].filterText = filter;
                    ApplyPaneFilter(paneIndex);
                }
                break;
            case ID_HEADER_NAME: if (HIWORD(wParam) == BN_CLICKED) { SetActivePane(paneIndex); SortPane(paneIndex, SortColumn::Name); } break;
            case ID_HEADER_EXT: if (HIWORD(wParam) == BN_CLICKED) { SetActivePane(paneIndex); SortPane(paneIndex, SortColumn::Extension); } break;
            case ID_HEADER_SIZE: if (HIWORD(wParam) == BN_CLICKED) { SetActivePane(paneIndex); SortPane(paneIndex, SortColumn::Size); } break;
            case ID_HEADER_DATE: if (HIWORD(wParam) == BN_CLICKED) { SetActivePane(paneIndex); SortPane(paneIndex, SortColumn::Modified); } break;
            }
        } else ExecuteCommand(id);
        return 0;
    }
    case WM_CONTEXTMENU: {
        HWND source = reinterpret_cast<HWND>(wParam);
        for (int paneIndex = 0; paneIndex < 2; ++paneIndex) {
            Pane& pane = g_app.panes[paneIndex];
            if (source == pane.tab) {
                ShowTabContextMenu(paneIndex, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            if (source != pane.list) continue;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1) {
                int item = ListView_GetNextItem(pane.list, -1, LVNI_FOCUSED);
                RECT itemRect{};
                if (item >= 0 && ListView_GetItemRect(pane.list, item, &itemRect, LVIR_BOUNDS)) {
                    point = {itemRect.left + Scale(24), itemRect.bottom};
                    ClientToScreen(pane.list, &point);
                } else {
                    RECT listRect{}; GetWindowRect(pane.list, &listRect);
                    point = {listRect.left + Scale(16), listRect.top + Scale(16)};
                }
            }
            ShowContextMenu(paneIndex, point);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: return HandleNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_DRAWITEM: {
        if (wParam == 0 && g_app.activeContextMenu3) {
            LRESULT menuResult = 0;
            if (SUCCEEDED(g_app.activeContextMenu3->HandleMenuMsg2(message, wParam, lParam, &menuResult))) return menuResult;
        } else if (wParam == 0 && g_app.activeContextMenu2 &&
                   SUCCEEDED(g_app.activeContextMenu2->HandleMenuMsg(message, wParam, lParam))) return TRUE;
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (DrawThemedMenuItem(draw)) return TRUE;
        DrawOwnerControl(draw);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, g_app.colors.muted); SetBkColor(dc, g_app.colors.surface);
        return reinterpret_cast<LRESULT>(g_app.surfaceBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, g_app.colors.text); SetBkColor(dc, g_app.colors.edit);
        return reinterpret_cast<LRESULT>(g_app.editBrush);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, g_app.windowBrush);
        const int contentInset = Scale(12), top = Scale(76), gap = Scale(6);
        RECT toolbarBand{0, 0, client.right, top};
        HBRUSH toolbarBrush = CreateSolidBrush(ToolbarColor());
        FillRect(dc, &toolbarBand, toolbarBrush); DeleteObject(toolbarBrush);
        RECT divider{0, top - Scale(1), client.right, top};
        HBRUSH dividerBrush = CreateSolidBrush(g_app.colors.border);
        FillRect(dc, &divider, dividerBrush); DeleteObject(dividerBrush);
        const int drawerHeight = OperationDrawerHeight();
        RECT content{contentInset, top, client.right - contentInset,
                     client.bottom - Scale(30) - drawerHeight - (drawerHeight ? gap : 0)};
        if (g_app.preferences.sidebarVisible) {
            const int available = static_cast<int>(content.right - content.left);
            const int sidebarWidth = (std::min)(Scale(216), (std::max)(Scale(160), available / 4));
            content.left += sidebarWidth;
        }
        const int count = g_app.dualPane ? 2 : 1;
        const int paneGap = g_app.dualPane ? gap : 0;
        const int paneWidth = (content.right - content.left - paneGap) / count;
        for (int i = 0; i < count; ++i) {
            RECT paneRect{content.left + i * (paneWidth + paneGap), content.top,
                          content.left + i * (paneWidth + paneGap) + paneWidth, content.bottom};
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN)); HGDIOBJ oldBrush = SelectObject(dc, g_app.surfaceBrush);
            RoundRect(dc, paneRect.left, paneRect.top, paneRect.right, paneRect.bottom, Scale(12), Scale(12));
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
        }
        if (drawerHeight) {
            RECT drawer{contentInset, client.bottom - Scale(30) - drawerHeight,
                        client.right - contentInset, client.bottom - Scale(30)};
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN)); HGDIOBJ oldBrush = SelectObject(dc, g_app.surfaceBrush);
            RoundRect(dc, drawer.left, drawer.top, drawer.right, drawer.bottom, Scale(14), Scale(14));
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
            const auto jobs = JobSnapshot(); auto primary = PrimaryOperationJob();
            size_t active = 0, queued = 0;
            for (const auto& job : jobs) { if (job->state == JobState::Running || job->state == JobState::Paused || job->state == JobState::Cancelling) ++active; else if (job->state == JobState::Queued) ++queued; }
            std::wstring title = active ? std::to_wstring(active) + L" active operation" + (active == 1 ? L"" : L"s") : L"File operations";
            if (queued) title += L"  ·  " + std::to_wstring(queued) + L" queued";
            RECT titleRect{drawer.left + Scale(16), drawer.top + Scale(8), drawer.right - Scale(260), drawer.top + Scale(31)};
            SelectObject(dc, g_app.fontBold); SetTextColor(dc, g_app.colors.text); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (primary) {
                std::wstring item;
                { std::lock_guard lock(primary->textMutex); item = primary->currentItem; }
                if (item.empty()) item = primary->sources.empty() ? primary->destination : primary->sources.front();
                RECT detail{drawer.left + Scale(16), drawer.top + Scale(30), drawer.right - Scale(260), drawer.top + Scale(51)};
                SelectObject(dc, g_app.font); SetTextColor(dc, g_app.colors.muted);
                const std::wstring detailText = std::wstring(JobKindText(primary->kind)) + L" · " + JobStateText(primary->state) + L" · " + item;
                DrawTextW(dc, detailText.c_str(), -1, &detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                RECT track{drawer.left + Scale(16), drawer.top + Scale(52), drawer.right - Scale(16), drawer.top + Scale(56)};
                HBRUSH trackBrush = CreateSolidBrush(g_app.colors.button); FillRect(dc, &track, trackBrush); DeleteObject(trackBrush);
                const UINT total = primary->progressTotal, done = primary->progressDone;
                const int progressWidth = primary->state == JobState::Completed ? static_cast<int>(track.right - track.left) :
                                          total ? static_cast<int>((track.right - track.left) * (std::min)(done, total) / total) : Scale(24);
                RECT progress{track.left, track.top, track.left + progressWidth, track.bottom};
                HBRUSH progressBrush = CreateSolidBrush(g_app.colors.selection); FillRect(dc, &progress, progressBrush); DeleteObject(progressBrush);
            }
            if (g_app.operations.expanded) {
                int y = drawer.top + Scale(68), shown = 0;
                for (auto it = jobs.rbegin(); it != jobs.rend() && shown < 4; ++it, ++shown) {
                    const auto& job = *it; std::wstring source = job->sources.empty() ? job->destination : job->sources.front();
                    std::wstring row = std::wstring(JobStateText(job->state)) + L"   " + JobKindText(job->kind) + L"   " + source;
                    RECT rowRect{drawer.left + Scale(16), y, drawer.right - Scale(16), y + Scale(23)};
                    SelectObject(dc, g_app.font); SetTextColor(dc, job->state == JobState::Failed ? RGB(210, 68, 68) : g_app.colors.text);
                    DrawTextW(dc, row.c_str(), -1, &rowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS); y += Scale(25);
                }
            }
        }
        RECT versionRect{client.left + Scale(12), client.bottom - Scale(26), client.right - Scale(12), client.bottom - Scale(6)};
        SelectObject(dc, g_app.font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, g_app.colors.muted);
        DrawTextW(dc, L"Files4Me-" FILES4ME_VERSION_DISPLAY_W, -1, &versionRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        EndPaint(window, &paint); return 0;
    }
    case WM_APP_UPDATE_DONE:
        HandleUpdateResult(std::unique_ptr<UpdateResult>(reinterpret_cast<UpdateResult*>(lParam)));
        return 0;
    case WM_APP_NEW_ITEM_DONE: {
        std::unique_ptr<NewItemResult> result(reinterpret_cast<NewItemResult*>(lParam));
        if (!result || result->pane < 0 || result->pane > 1) return 0;
        if (FAILED(result->result)) { ShowError(result->result, L"Create item"); return 0; }
        Pane& pane = g_app.panes[result->pane];
        if (pane.mode != PaneMode::Filesystem || pane.tabs.empty() ||
            _wcsicmp(ParentPath(result->path).c_str(), pane.tabs[pane.activeTab].path.c_str()) != 0) return 0;
        pane.pendingRenamePath = result->path;
        StartEnumeration(result->pane, pane.tabs[pane.activeTab].path);
        return 0;
    }
    case WM_APP_ENUM_DONE: {
        std::unique_ptr<EnumResult> result(reinterpret_cast<EnumResult*>(lParam));
        if (!result || result->pane < 0 || result->pane > 1) return 0;
        Pane& pane = g_app.panes[result->pane];
        if (result->generation != pane.generation.load()) return 0;
        pane.enumerating = false;
        if (pane.timeline) {
            pane.timelineItems = std::move(result->items);
            if (!pane.pendingRenamePath.empty()) {
                for (const FileItem& item : pane.timelineItems) {
                    if (_wcsicmp(item.fullPath.c_str(), pane.pendingRenamePath.c_str()) == 0) {
                        pane.collapsedTimelineGroups[TimelineBucketFor(item.modified)] = false; break;
                    }
                }
            }
            RebuildTimelineRows(pane);
        } else {
            pane.sourceItems = std::move(result->items);
            ApplyPaneFilter(result->pane, false);
        }
        WIN32_FILE_ATTRIBUTE_DATA directoryAttributes{};
        if (GetFileAttributesExW(ExtendedPath(result->path).c_str(), GetFileExInfoStandard, &directoryAttributes)) {
            pane.directoryStamp = directoryAttributes.ftLastWriteTime;
        }
        ApplyCutVisuals();
        if (result->pane == g_app.activePane) UpdateSelectionCommands();
        std::wstring status;
        if (result->error != ERROR_SUCCESS) status = L"Unable to read folder. Error " + std::to_wstring(result->error);
        else if (pane.filterText.empty()) status = std::to_wstring(pane.timeline ? pane.timelineItems.size() : pane.sourceItems.size()) + L" items";
        else status = std::to_wstring(std::count_if(pane.items.begin(), pane.items.end(), [](const FileItem& item) { return item.IsActionable(); })) +
                          L" of " + std::to_wstring(pane.timeline ? pane.timelineItems.size() : pane.sourceItems.size()) + L" items";
        SetWindowTextW(pane.status, status.c_str());
        InvalidateRect(pane.list, nullptr, TRUE);
        BeginPendingRename(pane);
        if (result->pane == g_app.activePane) UpdateSelectionCommands();
        return 0;
    }
    case WM_APP_ARCHIVE_DONE: {
        std::unique_ptr<ArchiveResult> result(reinterpret_cast<ArchiveResult*>(lParam));
        if (!result || result->pane < 0 || result->pane > 1) return 0;
        Pane& pane = g_app.panes[result->pane];
        if (result->generation != pane.generation.load() || pane.mode != PaneMode::Archive) return 0;
        pane.enumerating = false;
        if (FAILED(result->result)) {
            SetWindowTextW(pane.status, L"Unable to read ZIP archive");
            ShowError(result->result, L"Open ZIP archive"); return 0;
        }
        pane.sourceItems = std::move(result->items);
        ApplyPaneFilter(result->pane, false);
        SetWindowTextW(pane.status, (std::to_wstring(pane.items.size()) + L" items - read only").c_str());
        InvalidateRect(pane.list, nullptr, TRUE); LayoutWindow(); UpdateSelectionCommands(); return 0;
    }
    case WM_APP_ARCHIVE_EXTRACT_DONE: {
        std::unique_ptr<ArchiveExtractResult> result(reinterpret_cast<ArchiveExtractResult*>(lParam));
        if (!result) return 0;
        if (FAILED(result->result)) ShowError(result->result, L"Extract archive");
        else if (!result->openPath.empty()) {
            wchar_t executable[32768]{};
            const bool launchFiles4Me = result->openInFiles4Me &&
                                       GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)) != 0;
            const std::wstring parameters = L"\"" + result->openPath + L"\"";
            SHELLEXECUTEINFOW execute{sizeof(execute)}; execute.hwnd = window; execute.lpVerb = L"open";
            execute.lpFile = launchFiles4Me ? executable : result->openPath.c_str();
            execute.lpParameters = launchFiles4Me ? parameters.c_str() : nullptr;
            execute.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&execute)) ShowError(HRESULT_FROM_WIN32(GetLastError()), L"Open extracted item");
        }
        if (result->pane >= 0 && result->pane < 2) SetWindowTextW(g_app.panes[result->pane].status, L"Extraction complete");
        return 0;
    }
    case WM_APP_DRIVES_DONE: {
        std::unique_ptr<DriveResult> result(reinterpret_cast<DriveResult*>(lParam));
        if (!result || result->pane < 0 || result->pane > 1) return 0;
        Pane& pane = g_app.panes[result->pane];
        if (result->generation != pane.generation.load() || pane.mode != PaneMode::DriveOverview) return 0;
        pane.enumerating = false; pane.driveItems = std::move(result->items);
        ListView_SetView(pane.list, LV_VIEW_TILE);
        ListView_SetImageList(pane.list, g_app.systemImagesLarge, LVSIL_NORMAL);
        ConfigureTileView(pane);
        ListView_SetItemCountEx(pane.list, static_cast<int>(pane.driveItems.size()), 0);
        const std::wstring status = std::to_wstring(pane.driveItems.size()) + L" drives";
        SetWindowTextW(pane.status, status.c_str());
        InvalidateRect(pane.list, nullptr, TRUE); LayoutWindow();
        return 0;
    }
    case WM_APP_JOB_UPDATE:
        UpdateOperationButtons(); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_APP_JOB_DONE: {
        std::unique_ptr<std::shared_ptr<OperationJob>> completed(reinterpret_cast<std::shared_ptr<OperationJob>*>(lParam));
        if (!completed || !*completed) return 0;
        {
            std::lock_guard lock(g_app.operations.mutex);
            for (const auto& root : (*completed)->resourceKeys) g_app.operations.activeRoots.erase(root);
            if (g_app.operations.activeCount) --g_app.operations.activeCount;
        }
        if ((*completed)->kind == JobKind::NewFolder && FAILED((*completed)->result.load())) {
            const std::wstring failedPath = JoinPath((*completed)->destination, (*completed)->newName);
            for (Pane& pane : g_app.panes)
                if (_wcsicmp(pane.pendingRenamePath.c_str(), failedPath.c_str()) == 0) pane.pendingRenamePath.clear();
        }
        SaveJobJournal(); RefreshAll(); PumpOperationQueue(); LayoutWindow();
        if (g_app.closingAfterOperations) {
            std::lock_guard lock(g_app.operations.mutex);
            if (g_app.operations.activeCount == 0) PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE: {
        size_t active = 0;
        { std::lock_guard lock(g_app.operations.mutex); active = g_app.operations.activeCount; }
        if (active && !g_app.closingAfterOperations) {
            if (MessageBoxW(window, L"File operations are active. Cancel them and exit when safe?", kAppName,
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
            g_app.closingAfterOperations = true;
            const auto jobs = JobSnapshot();
            for (const auto& job : jobs) {
                if (job->state == JobState::Running || job->state == JobState::Paused || job->state == JobState::Cancelling) {
                    job->cancelRequested = true; job->pauseRequested = false; job->state = JobState::Cancelling; job->controlChanged.notify_all();
                } else if (job->state == JobState::Queued) { job->result = HRESULT_FROM_WIN32(ERROR_CANCELLED); job->state = JobState::Failed; }
            }
            SaveJobJournal(); LayoutWindow(); return 0;
        }
        SaveSettings(); SaveJobJournal(); DestroyWindow(window); return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        RemoveClipboardFormatListener(window);
        for (Pane& pane : g_app.panes) {
            ++pane.generation;
            if (pane.dropTarget) {
                RevokeDragDrop(pane.list);
                pane.dropTarget->Release();
                pane.dropTarget = nullptr;
            }
        }
        ReleaseBrush(g_app.windowBrush); ReleaseBrush(g_app.surfaceBrush); ReleaseBrush(g_app.editBrush);
        if (g_app.font) DeleteObject(g_app.font); if (g_app.fontBold) DeleteObject(g_app.fontBold);
        if (g_app.fileFont) DeleteObject(g_app.fileFont);
        if (g_app.iconFont) DeleteObject(g_app.iconFont);
        for (IconCache& cache : g_app.iconCaches) if (cache.images) ImageList_Destroy(cache.images);
        if (g_app.smallIconCache.images) ImageList_Destroy(g_app.smallIconCache.images);
        if (g_app.sidebarImages) ImageList_Destroy(g_app.sidebarImages);
        if (g_app.checkboxImages) ImageList_Destroy(g_app.checkboxImages);
        if (g_app.materialFontResource) RemoveFontMemResourceEx(g_app.materialFontResource);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HACCEL CreateAccelerators() {
    ACCEL keys[] = {
        {FVIRTKEY, VK_F2, ID_RENAME}, {FVIRTKEY, VK_F5, ID_COPY}, {FVIRTKEY, VK_F6, ID_MOVE},
        {FVIRTKEY, VK_F7, ID_CREATE_FOLDER}, {FVIRTKEY, VK_F8, ID_DELETE}, {FVIRTKEY, VK_DELETE, ID_DELETE},
        {static_cast<BYTE>(FVIRTKEY | FSHIFT), VK_DELETE, ID_DELETE_PERMANENT},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'T', ID_NEW_TAB}, {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'W', ID_CLOSE_TAB},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'L', ID_FOCUS_PATH}, {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'F', ID_FOCUS_SEARCH},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'A', ID_SELECT_ALL},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'C', ID_CLIP_COPY}, {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'X', ID_CLIP_CUT},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'V', ID_CLIP_PASTE},
        {static_cast<BYTE>(FVIRTKEY | FALT), 'F', ID_MENU_FILE}, {static_cast<BYTE>(FVIRTKEY | FALT), 'E', ID_MENU_EDIT},
        {static_cast<BYTE>(FVIRTKEY | FALT), 'V', ID_MENU_VIEW}, {static_cast<BYTE>(FVIRTKEY | FALT), 'H', ID_MENU_HELP},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_LEFT, ID_BACK}, {static_cast<BYTE>(FVIRTKEY | FALT), VK_RIGHT, ID_FORWARD},
        {static_cast<BYTE>(FVIRTKEY | FALT), VK_UP, ID_UP}
    };
    return CreateAcceleratorTableW(keys, ARRAYSIZE(keys));
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    g_app.instance = instance;
    std::wstring startupPath;
    int argumentCount = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount)) {
        if (argumentCount > 1) {
            startupPath = NormalizePath(arguments[1]);
            const DWORD attributes = startupPath.empty() ? INVALID_FILE_ATTRIBUTES
                : GetFileAttributesW(ExtendedPath(startupPath).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) startupPath.clear();
            else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) startupPath = ParentPath(startupPath);
        }
        LocalFree(arguments);
    }
    const std::wstring stateDirectory = ResolveStateDirectory();
    g_app.stateDirectory = stateDirectory;
    g_app.iniPath = JoinPath(stateDirectory, L"Files4Me.ini");
    g_app.jobsPath = JoinPath(stateDirectory, L"Files4Me.jobs");
    LoadPreferences();
    LoadJobJournal();
    g_app.theme = _wcsicmp(ReadSetting(L"Appearance", L"Theme", L"Dark").c_str(), L"Dark") == 0 ? ThemeMode::Dark : ThemeMode::Light;
    g_app.dualPane = GetPrivateProfileIntW(L"Layout", L"DualPane", 1, g_app.iniPath.c_str()) != 0;
    g_app.activePane = GetPrivateProfileIntW(L"Layout", L"ActivePane", 0, g_app.iniPath.c_str()) == 1 ? 1 : 0;

    HRESULT comResult = OleInitialize(nullptr);
    if (FAILED(comResult)) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) { OleUninitialize(); return 1; }
    WNDCLASSEXW settingsClass{sizeof(settingsClass)};
    settingsClass.lpfnWndProc = SettingsWindowProc;
    settingsClass.hInstance = instance;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&settingsClass)) { OleUninitialize(); return 1; }
    WNDCLASSEXW layoutClass{sizeof(layoutClass)};
    layoutClass.lpfnWndProc = LayoutFlyoutProc;
    layoutClass.hInstance = instance;
    layoutClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    layoutClass.lpszClassName = kLayoutClass;
    if (!RegisterClassExW(&layoutClass)) { OleUninitialize(); return 1; }

    int width = (std::max)(static_cast<int>(GetPrivateProfileIntW(L"Window", L"Width", 1180, g_app.iniPath.c_str())), 760);
    int height = (std::max)(static_cast<int>(GetPrivateProfileIntW(L"Window", L"Height", 720, g_app.iniPath.c_str())), 480);
    POINT cursor{}; GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{sizeof(monitorInfo)}; GetMonitorInfoW(monitor, &monitorInfo);
    const RECT work = monitorInfo.rcWork;
    width = (std::min)(width, static_cast<int>(work.right - work.left));
    height = (std::min)(height, static_cast<int>(work.bottom - work.top));
    const int x = work.left + (static_cast<int>(work.right - work.left) - width) / 2;
    const int y = work.top + (static_cast<int>(work.bottom - work.top) - height) / 2;
    HWND window = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) { OleUninitialize(); return 1; }
    ShowWindow(window, showCommand); UpdateWindow(window);
    if (!startupPath.empty()) Navigate(g_app.activePane, startupPath, false);

    HACCEL accelerators = CreateAccelerators();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool bypassAccelerators = false;
        if (message.message == WM_KEYDOWN) {
            HWND focus = GetFocus();
            for (Pane& pane : g_app.panes) {
                if (focus == pane.search) {
                    if (message.wParam == VK_ESCAPE) {
                        SetWindowTextW(pane.search, L"");
                        SetFocus(pane.list);
                        bypassAccelerators = true;
                    } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && message.wParam != 'L') {
                        bypassAccelerators = true;
                    }
                    break;
                }
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (message.wParam == VK_BACK || message.wParam == VK_DELETE)) {
                for (const Pane& pane : g_app.panes)
                    if (focus == pane.path || focus == pane.search) { bypassAccelerators = true; break; }
            }
            if (g_app.layoutPopup && IsWindow(g_app.layoutPopup) &&
                (focus == g_app.layoutPopup || IsChild(g_app.layoutPopup, focus))) {
                int current = 0;
                for (size_t index = 0; index < g_app.layoutButtons.size(); ++index)
                    if (focus == g_app.layoutButtons[index]) current = static_cast<int>(index);
                if (message.wParam == VK_ESCAPE) { DestroyWindow(g_app.layoutPopup); continue; }
                if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
                    SendMessageW(g_app.layoutPopup, WM_COMMAND, GetDlgCtrlID(g_app.layoutButtons[current]),
                                 reinterpret_cast<LPARAM>(g_app.layoutButtons[current]));
                    continue;
                }
                int next = current;
                if (message.wParam == VK_TAB || message.wParam == VK_RIGHT) next = (current + 1) % 8;
                else if (message.wParam == VK_LEFT) next = (current + 7) % 8;
                else if (message.wParam == VK_DOWN) next = (current + 3) % 8;
                else if (message.wParam == VK_UP) next = (current + 5) % 8;
                if (next != current) { SetFocus(g_app.layoutButtons[next]); continue; }
            }
            if (message.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
                ExecuteCommand(ID_SWITCH_PANE); continue;
            }
            if (message.wParam == VK_RETURN) {
                bool handled = false;
                for (int i = 0; i < 2; ++i) {
                    if (focus == g_app.panes[i].path) {
                        wchar_t value[32768]{}; GetWindowTextW(focus, value, ARRAYSIZE(value));
                        SetActivePane(i); Navigate(i, value); handled = true; break;
                    }
                    if (focus == g_app.panes[i].search) { SetFocus(g_app.panes[i].list); handled = true; break; }
                    if (focus == g_app.panes[i].list) { SetActivePane(i); OpenFocused(); handled = true; break; }
                }
                if (handled) continue;
            }
            if (message.wParam == VK_INSERT) {
                Pane& pane = g_app.panes[g_app.activePane];
                int item = ListView_GetNextItem(pane.list, -1, LVNI_FOCUSED);
                if (item >= 0) {
                    UINT state = ListView_GetItemState(pane.list, item, LVIS_SELECTED);
                    ListView_SetItemState(pane.list, item, state ? 0 : LVIS_SELECTED, LVIS_SELECTED);
                    ListView_SetItemState(pane.list, item + 1, LVIS_FOCUSED, LVIS_FOCUSED);
                }
                continue;
            }
        }
        if (bypassAccelerators || !TranslateAcceleratorW(window, accelerators, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    if (accelerators) DestroyAcceleratorTable(accelerators);
    OleUninitialize();
    return static_cast<int>(message.wParam);
}
