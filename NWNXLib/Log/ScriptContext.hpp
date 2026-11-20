#pragma once

#include <cstdint>

namespace NWNXLib::Log {

class Record;

// Keys of the automatic script context, as bits for SetScriptContextKeys().
namespace ContextKey
{
    enum : uint32_t
    {
        Script  = 1 << 0,
        Oid     = 1 << 1,
        Tag     = 1 << 2,
        Area    = 1 << 3,
        Pc      = 1 << 4,
        Player  = 1 << 5,
        Default = Script | Oid | Tag | Area,
    };
}

// Adds the enabled keys of the running script (script, oid, tag, area, pc, player) to the record. Does
// nothing off the main thread or outside a script. Core installs it with SetContextSource().
void AddScriptContext(Record& record);

void SetScriptContextKeys(uint32_t keys);
uint32_t GetScriptContextKeys();

}
