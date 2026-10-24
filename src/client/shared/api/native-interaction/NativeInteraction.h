#pragma once
#include <cstdint>
namespace coop {
bool NativeInteractionAllowed(void*script,int button);
uint8_t NativeInteractionOwner(uint64_t key);
uint64_t NativeInteractionKey(uint16_t model,int8_t area,float x,float y,float z);
bool RequestNativeInteraction(uint16_t model,int8_t area,float x,float y,float z);
void ReleaseNativeInteraction();
bool BeginNativeInteraction(void*script);
void EndNativeInteraction(void*script);
}
