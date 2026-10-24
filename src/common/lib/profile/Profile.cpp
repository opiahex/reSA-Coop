#include <windows.h>
#include "common/lib/profile/Profile.h"
namespace coop::profile {
uint64_t Micros(){static const uint64_t frequency=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return (uint64_t)f.QuadPart;}();LARGE_INTEGER t;QueryPerformanceCounter(&t);auto ticks=(uint64_t)t.QuadPart;return ticks/frequency*1000000+(ticks%frequency)*1000000/frequency;}
}
