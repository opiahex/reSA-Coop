#pragma once
namespace coop {
// Resolve the original target before the native command can replace its model.
bool ResolveStoryMapTarget(int opcode,int* parameters);
// Only the authoritative story interpreter calls this after a native map edit.
void RecordStoryMapCommand(int opcode,const int* parameters);
}
