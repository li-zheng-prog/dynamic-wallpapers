#pragma once
#include <windows.h>
#include <shellapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <string>
#include "resource.h"

// Keep the IDs stable across upgrades; change only the icon file name when the artwork changes.
inline constexpr wchar_t kShellAppId[] = L"DynamicWallpapers.Windows";
inline constexpr wchar_t kShellSetupId[] = L"DynamicWallpapers.Windows.Setup";
inline constexpr wchar_t kShellIconFile[] = L"DynamicWallpapers-logo-v5.ico";

inline HRESULT SetShellString(IPropertyStore* store, REFPROPERTYKEY key, const std::wstring& text) {
    PROPVARIANT value = {};
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<wchar_t*>(text.c_str());
    return store->SetValue(key, value);
}

inline void SetShellWindowRelaunch(HWND hwnd, const std::wstring& exe, const std::wstring& icon,
                                   const wchar_t* args, int title, const wchar_t* appId) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, (void**)&store))) return;
    SetShellString(store, PKEY_AppUserModel_RelaunchCommand, L"\"" + exe + L"\"" + args);
    SetShellString(store, PKEY_AppUserModel_RelaunchIconResource, icon);
    SetShellString(store, PKEY_AppUserModel_RelaunchDisplayNameResource,
                   L"@" + exe + L",-" + std::to_wstring(title));
    // The shell reads the preceding properties when the window's ID is set.
    SetShellString(store, PKEY_AppUserModel_ID, appId);
    store->Release();
}
