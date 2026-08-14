#pragma once

// Single native release identity used by the executable, resources, and updater.
#define FILES4ME_VERSION_NUMERIC 1,0,0,0
#define FILES4ME_VERSION_DISPLAY_A "1.0-release"
#define FILES4ME_VERSION_DISPLAY_W L"1.0-release"
#define FILES4ME_VERSION_SEMVER_W L"1.0.0"

// Release builds must opt into their update stream explicitly. Change both
// values to "stable"/L"stable" when preparing a stable release.
#define FILES4ME_UPDATE_CHANNEL_A "stable"
#define FILES4ME_UPDATE_CHANNEL_W L"stable"
