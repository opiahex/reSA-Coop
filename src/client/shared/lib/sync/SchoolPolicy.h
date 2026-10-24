#pragma once
#include <cctype>
#include <cstring>

namespace coop {
// Native SCM names, including the flight school's main-script controller.
inline bool IsSchoolScriptName(const char* name) {
    if (!name) return false;
    static const char* const names[]={"dskool", "psch", "bskool", "boat", "bschoo", "bikes"};
    for (const char* school : names) {
        size_t i=0;
        while (i<8 && school[i] && std::tolower((unsigned char)name[i])==school[i]) ++i;
        if (!school[i] && (i==8 || !name[i])) return true;
    }
    return false;
}
}
