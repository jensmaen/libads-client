// Text helpers shared by the symbol browser examples: values of PLC elements as text (UTF-8), AmsNetIds, ADS states
// and which symbols come from Beckhoff's libraries. Header only, no platform API.
#pragma once

#include "AdsLib.h"
#include "AdsSymbols.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace symbol_text {

template <typename... Args> std::string Format(const char* format, Args... args) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return buffer;
}

inline std::string Upper(std::string s) {
    for (auto& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

inline std::string NetIdText(const AmsNetId& id) {
    return Format("%u.%u.%u.%u.%u.%u", id.b[0], id.b[1], id.b[2], id.b[3], id.b[4], id.b[5]);
}

inline bool ParseNetId(const std::string& text, AmsNetId& id) {
    const char* p = text.c_str();
    for (int i = 0; i < 6; ++i) {
        if (*p < '0' || *p > '9') return false;
        char* end;
        const unsigned long b = std::strtoul(p, &end, 10);
        if (b > 255 || *end != (i < 5 ? '.' : '\0')) return false;
        id.b[i] = static_cast<uint8_t>(b);
        p = end + 1;
    }
    return true;
}

inline const char* StateName(ADSSTATE s) {
    switch (s) {
    case ADSSTATE_INVALID: return "Invalid";
    case ADSSTATE_IDLE: return "Idle";
    case ADSSTATE_RESET: return "Reset";
    case ADSSTATE_INIT: return "Init";
    case ADSSTATE_START: return "Start";
    case ADSSTATE_RUN: return "Run";
    case ADSSTATE_STOP: return "Stop";
    case ADSSTATE_CONFIG: return "Config";
    case ADSSTATE_RECONFIG: return "Reconfig";
    default: return "State";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Unicode

inline void AppendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Code page 1252 (the default of STRING in TwinCAT) to UTF-8
inline std::string Utf8FromCp1252(const std::string& bytes) {
    // 0x80 to 0x9F; the rest matches Latin-1. 0xFFFD where 1252 has no character.
    static const uint16_t high[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
    };
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<uint8_t>(ch);
        AppendUtf8(out, c >= 0x80 && c < 0xA0 ? high[c - 0x80] : c);
    }
    return out;
}

// UTF-16 little endian (WSTRING) up to the first 0 to UTF-8
inline std::string Utf8FromUtf16(const std::vector<uint8_t>& d) {
    std::string out;
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        uint32_t c = d[i] | static_cast<uint32_t>(d[i + 1]) << 8;
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < d.size()) {
            const uint32_t low = d[i + 2] | static_cast<uint32_t>(d[i + 3]) << 8;
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, c >= 0xD800 && c < 0xE000 ? 0xFFFD : c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Values as text. The library hands out raw bytes; how to read them follows from the kind and type of the element.

inline uint64_t Unsigned(const std::vector<uint8_t>& d, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < d.size() && i < 8; ++i) v |= static_cast<uint64_t>(d[i]) << (8 * i);
    return v;
}

inline int64_t Signed(const std::vector<uint8_t>& d, size_t n) {
    uint64_t v = Unsigned(d, n);
    if (n > 0 && n < 8 && ((v >> (8 * n - 1)) & 1)) v |= ~uint64_t{0} << (8 * n);
    return static_cast<int64_t>(v);
}

inline bool IsSigned(uint32_t dataType) {
    return dataType == ADST_INT8 || dataType == ADST_INT16 || dataType == ADST_INT32 || dataType == ADST_INT64;
}

// Days since 1970-01-01 to year, month, day (proleptic Gregorian calendar)
inline void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

inline std::string Duration(uint64_t ms, const char* prefix) {
    std::string s = prefix;
    const uint64_t parts[] = {ms / 86400000, ms / 3600000 % 24, ms / 60000 % 60, ms / 1000 % 60, ms % 1000};
    const char* units[] = {"d", "h", "m", "s", "ms"};
    for (int i = 0; i < 5; ++i) {
        if (parts[i]) s += std::to_string(parts[i]) + units[i];
    }
    return s == prefix ? s + "0ms" : s;
}

inline std::string ValueText(const AdsElement& e, const std::vector<uint8_t>& d) {
    using Kind = AdsElement::Kind;
    switch (e.kind) {
    case Kind::BitField: return d.empty() ? "" : ((d[0] >> e.bitOffset) & 1) ? "TRUE" : "FALSE";
    case Kind::Pointer:
    case Kind::Reference:
    case Kind::Interface: return Format("16#%llX", static_cast<unsigned long long>(Unsigned(d, d.size())));
    case Kind::String: {
        const auto end = std::find(d.begin(), d.end(), uint8_t{0});
        const std::string bytes(d.begin(), end);
        for (const auto& a : e.attributes) {
            if (Upper(a.name) == "TCENCODING" && Upper(a.value) == "UTF-8") return "'" + bytes + "'";
        }
        return "'" + Utf8FromCp1252(bytes) + "'";
    }
    case Kind::WString: return "\"" + Utf8FromUtf16(d) + "\"";
    case Kind::Enum: {
        const auto t = e.type;
        const int64_t v = t && IsSigned(t->dataType) ? Signed(d, d.size()) : static_cast<int64_t>(Unsigned(d, d.size()));
        if (t) {
            for (const auto& ev : t->enumValues) {
                if (ev.value == v) return ev.name + Format(" (%lld)", static_cast<long long>(v));
            }
        }
        return Format("%lld", static_cast<long long>(v));
    }
    default: break;
    }

    const std::string type = Upper(e.type ? e.type->name : e.typeName);
    const uint64_t u = Unsigned(d, d.size());
    if (type == "BOOL" || e.dataType == ADST_BIT) return u ? "TRUE" : "FALSE";
    if (type == "TIME") return Duration(u, "T#");
    if (type == "LTIME") return Duration(u / 1000000, "LTIME#") + Format(" +%lluns", static_cast<unsigned long long>(u % 1000000));
    if (type == "DATE" || type == "DATE_AND_TIME" || type == "DT") {
        int y;
        unsigned m, day;
        CivilFromDays(static_cast<int64_t>(u / 86400), y, m, day);
        if (type == "DATE") return Format("D#%04d-%02u-%02u", y, m, day);
        const auto s = u % 86400;
        return Format("DT#%04d-%02u-%02u-%02llu:%02llu:%02llu", y, m, day, static_cast<unsigned long long>(s / 3600),
                      static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
    }
    if (type == "TIME_OF_DAY" || type == "TOD") {
        return Format("TOD#%02llu:%02llu:%02llu.%03llu", static_cast<unsigned long long>(u / 3600000),
                      static_cast<unsigned long long>(u / 60000 % 60), static_cast<unsigned long long>(u / 1000 % 60),
                      static_cast<unsigned long long>(u % 1000));
    }
    if (type == "BYTE" || type == "WORD" || type == "DWORD" || type == "LWORD") {
        return Format("16#%llX", static_cast<unsigned long long>(u));
    }
    switch (e.type ? e.type->dataType : e.dataType) {
    case ADST_INT8:
    case ADST_INT16:
    case ADST_INT32:
    case ADST_INT64: return Format("%lld", static_cast<long long>(Signed(d, d.size())));
    case ADST_UINT8:
    case ADST_UINT16:
    case ADST_UINT32:
    case ADST_UINT64: return Format("%llu", static_cast<unsigned long long>(u));
    case ADST_REAL32: {
        float f;
        if (d.size() < sizeof(f)) break;
        std::memcpy(&f, d.data(), sizeof(f));
        return Format("%.7g", static_cast<double>(f));
    }
    case ADST_REAL64: {
        double f;
        if (d.size() < sizeof(f)) break;
        std::memcpy(&f, d.data(), sizeof(f));
        return Format("%.15g", f);
    }
    default: break;
    }
    std::string hex;
    for (size_t i = 0; i < d.size() && i < 32; ++i) hex += Format("%02X ", d[i]);
    if (d.size() > 32) hex += "...";
    return hex;
}

// Variables that come from Beckhoff's libraries or TwinCAT itself, not from the project. The upload does not mark
// them, so this goes by the names of their global variable lists.
inline bool IsLibrarySymbol(const std::string& name) {
    const std::string top = Upper(name.substr(0, name.find('.')));
    static const char* const lists[] = {"GLOBAL_VERSION", "GLOBAL_VARIABLES", "CONSTANTS", "LREAL_CONST"};
    for (const auto l : lists) {
        if (top == l) return true;
    }
    return top.compare(0, 8, "TWINCAT_") == 0;
}

// The name of a tree row below its symbol: "stAxis", "[3]"; the symbol itself (depth 0) keeps its full name.
inline std::string RowName(const std::string& path, int depth) {
    if (depth == 0) return path;
    const size_t dot = path.find_last_of('.');
    const size_t bracket = path.find_last_of('[');
    const size_t cut = (bracket != std::string::npos && (dot == std::string::npos || bracket > dot)) ? bracket : dot + 1;
    return path.substr(cut);
}

} // namespace symbol_text
