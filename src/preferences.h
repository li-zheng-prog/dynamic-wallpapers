#pragma once
#include <string>

enum class AppLanguage { System, Chinese, English };

inline AppLanguage ParseAppLanguage(const std::wstring& tag) {
    if (tag == L"zh-CN") return AppLanguage::Chinese;
    if (tag == L"en-US") return AppLanguage::English;
    return AppLanguage::System;
}

inline const wchar_t* AppLanguageTag(AppLanguage language) {
    if (language == AppLanguage::Chinese) return L"zh-CN";
    if (language == AppLanguage::English) return L"en-US";
    return L"system";
}

inline bool ResolveAppLanguage(AppLanguage language, bool windowsChinese) {
    return language == AppLanguage::System ? windowsChinese : language == AppLanguage::Chinese;
}

// Keep Unicode font names lossless even in existing ANSI INIs.
inline std::wstring EncodeFontPreference(const std::wstring& family) {
    if (family.empty()) return L"";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result = L"u16:";
    for (unsigned int unit : family)
        for (int shift = 12; shift >= 0; shift -= 4)
            result += hex[(unit >> shift) & 15];
    return result;
}

inline std::wstring DecodeFontPreference(const std::wstring& value) {
    if (value.compare(0, 4, L"u16:") != 0) return value;
    if ((value.size() - 4) % 4 != 0) return L"";
    std::wstring family;
    for (size_t i = 4; i < value.size();) {
        unsigned int unit = 0;
        for (int j = 0; j < 4; ++j) {
            const wchar_t ch = value[i++];
            if (ch >= L'0' && ch <= L'9') unit = unit * 16 + ch - L'0';
            else if (ch >= L'a' && ch <= L'f') unit = unit * 16 + ch - L'a' + 10;
            else return L"";
        }
        if (!unit) return L"";
        family += static_cast<wchar_t>(unit);
    }
    return family;
}
