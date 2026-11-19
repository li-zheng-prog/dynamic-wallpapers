#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <string>
#include "resource.h"

inline constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers";
inline constexpr wchar_t kInstallLanguageValue[] = L"InstallLanguage";

inline bool WindowsPrefersChinese() {
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 1) {
        std::wstring preferred(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, preferred.data(), &size) && count) {
            wchar_t language[9] = {};
            if (GetLocaleInfoEx(preferred.c_str(), LOCALE_SISO639LANGNAME, language, 9))
                return lstrcmpiW(language, L"zh") == 0;
        }
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

inline const wchar_t* InstallLanguageTag(bool chinese) {
    return chinese ? L"zh-CN" : L"en-US";
}

inline bool ResolveInstallLanguage(const wchar_t* tag, bool windowsChinese) {
    if (lstrcmpiW(tag, InstallLanguageTag(true)) == 0) return true;
    if (lstrcmpiW(tag, InstallLanguageTag(false)) == 0) return false;
    return windowsChinese;
}

inline bool ReadInstallLanguage(HKEY key, bool windowsChinese) {
    wchar_t tag[16] = {};
    DWORD size = sizeof(tag);
    if (!key || RegGetValueW(key, nullptr, kInstallLanguageValue, RRF_RT_REG_SZ,
                            nullptr, tag, &size) != ERROR_SUCCESS)
        tag[0] = L'\0';
    return ResolveInstallLanguage(tag, windowsChinese);
}

inline LONG WriteInstallLanguage(HKEY key, bool chinese) {
    const wchar_t* tag = InstallLanguageTag(chinese);
    const DWORD bytes = (lstrlenW(tag) + 1) * sizeof(wchar_t);
    return RegSetValueExW(key, kInstallLanguageValue, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(tag), bytes);
}

inline bool InstalledLanguageIsChinese() {
    const bool windowsChinese = WindowsPrefersChinese();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return windowsChinese;
    const bool chinese = ReadInstallLanguage(key, windowsChinese);
    RegCloseKey(key);
    return chinese;
}

// Set once at application startup. Core helpers use the same language as the window and tray.
inline bool g_appChinese = false;
inline const wchar_t* AppText(const wchar_t* english, const wchar_t* chinese) {
    return g_appChinese ? chinese : english;
}

inline bool InstallDialog(HWND owner, const wchar_t* title, const wchar_t* headline,
                          const wchar_t* body, PCWSTR icon, const wchar_t* action,
                          const wchar_t* cancel = nullptr) {
    const TASKDIALOG_BUTTON buttons[] = {{IDOK, action}, {IDCANCEL, cancel}};
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = title;
    config.pszMainInstruction = headline;
    config.pszContent = body;
    config.pszMainIcon = icon;
    config.cButtons = cancel ? 2 : 1;
    config.pButtons = buttons;
    config.nDefaultButton = cancel ? IDCANCEL : IDOK;
    // Enable themed controls only for these dialogs; leave the executable manifest unchanged.
    ACTCTXW context = {};
    context.cbSize = sizeof(context);
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = GetModuleHandleW(nullptr);
    context.lpResourceName = MAKEINTRESOURCEW(IDR_DIALOG_MANIFEST);
    HANDLE handle = CreateActCtxW(&context);
    HRESULT result = E_FAIL;
    int clicked = IDCANCEL;
    if (handle != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie = 0;
        if (ActivateActCtx(handle, &cookie)) {
            if (HMODULE controls = LoadLibraryW(L"comctl32.dll")) {
                // GetProcAddress returns a generic function pointer; copy its representation.
                auto proc = GetProcAddress(controls, "TaskDialogIndirect");
                decltype(&TaskDialogIndirect) show = nullptr;
                static_assert(sizeof(show) == sizeof(proc));
                std::memcpy(&show, &proc, sizeof(show));
                if (show) result = show(&config, &clicked, nullptr, nullptr);
                FreeLibrary(controls);
            }
            DeactivateActCtx(0, cookie);
        }
        ReleaseActCtx(handle);
    }
    if (SUCCEEDED(result)) return clicked == IDOK;

    if (cancel) {
        std::wstring prompt = std::wstring(headline) + L"\n\n" + body;
        return MessageBoxW(owner, prompt.c_str(), title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }
    MessageBoxW(owner, body ? body : headline, title,
                MB_OK | (icon == TD_ERROR_ICON ? MB_ICONERROR : MB_ICONINFORMATION));
    return true;
}
