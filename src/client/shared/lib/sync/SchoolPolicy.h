#pragma once
#include <cctype>
#include <cstring>

namespace coop {
// SCM names occupy eight bytes and need not end in a null character.
inline bool NativeActivityNameMatches(const char* name,const char* expected,bool prefix=false) {
    if(!name)return false;
    size_t i=0;
    while(i<8&&expected[i]&&std::tolower((unsigned char)name[i])==expected[i])++i;
    return !expected[i]&&(prefix||i==8||!name[i]);
}
// Native SCM names, including the flight school's main-script controller.
inline bool IsSchoolScriptName(const char* name) {
    if (!name) return false;
    static const char* const names[]={"dskool", "psch", "desert5", "bskool", "boat", "bschoo", "bikes"};
    for (const char* school : names) if(NativeActivityNameMatches(name,school))return true;
    return false;
}

// One name policy for script ownership and the native live-attempt gate.
// DANCE is the streamed player minigame; DANCER is an ambient NPC script.
// CPRACE also runs the two BCESAR story races. The SCM caller distinguishes
// those ($484=0) from personal tournaments ($484=1).
inline bool IsPersonalActivityScriptName(const char* name,bool raceTournament=true) {
    if(NativeActivityNameMatches(name,"cprace"))return raceTournament;
    static const char*const names[]={"none", "dual", "shtr", "grav", "otb", "pool2", "lowr", "zero5", "truck", "boat", "blood", "tria", "bcour", "stunt", "oddveh", "taxiodd", "ambulan", "firetru", "copcar", "pimp", "freight", "trainsl",
        "trace", "hotr", "kicks", "kicksta", "bloodr", "dskool", "bskool", "bschoo", "psch", "desert5", "bikes",
        "gym", "gymls", "gymsf", "gymlv", "shoot", "shrange", "trucks", "quarry", "quarrys", "burgjb", "valet_l", "mtbiker", "bbchal", "barb", "tatto", "blackj", "roulete", "bandit", "vidpok", "wheelo", "dance"};
    static const char*const prefixes[]={"gym","shop","food","pizza","chick","burger","barber","tattoo","wardrob","warrobe","psave","buypro","intman","entext","barguy","barstaf","amunat","browse","car_mod","carmod","vending","fodvend","hotdog","gfdate","gfmeet"};
    for(const char*prefix:prefixes)if(NativeActivityNameMatches(name,prefix,true))return true;
    for(const char*activity:names)if(NativeActivityNameMatches(name,activity))return true;
    return false;
}
}
