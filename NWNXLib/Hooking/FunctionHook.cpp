#include "Hooking/FunctionHook.hpp"
#include "Assert.hpp"
#include "External/subhook/subhook.h"
#include <cstring>
#include <Zycore/Types.h>
#include <Zydis/Zydis.h>
#include <Zycore/Status.h>
namespace NWNXLib {

namespace Hooking {

using namespace NWNXLib::Platform;

subhook_disasm_handler_t FunctionHook::ZydisDisassemble(void *src, int *reloc_op_offset)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);

    auto runtime_address = (ZyanU64)src;
    ZyanUSize offset = 0;
    const ZyanUSize length = sizeof(src);
    ZydisDecodedInstruction instruction;
    while (ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder, (const ZyanU64*)src + offset, length - offset, &instruction)))
    {
        offset += instruction.length;
        runtime_address += instruction.length;
        if (offset >= 14) break;
    }
    return nullptr;
}

FunctionHook::FunctionHook(uintptr_t originalFunction, uintptr_t newFunction)
{
    m_subhook = subhook_new((void*)originalFunction, (void*)newFunction, SUBHOOK_64BIT_OFFSET);
    ASSERT(m_subhook);
    subhook_install(m_subhook);
    int reloc_op_offset = 0;
    m_zydisDisasm = ZydisDisassemble((void*)originalFunction, &reloc_op_offset);
    subhook_set_disasm_handler(m_zydisDisasm);
    m_trampoline = subhook_get_trampoline(m_subhook);
    ASSERT(m_trampoline);
}

FunctionHook::~FunctionHook()
{
    subhook_remove(m_subhook);
    subhook_free(m_subhook);
}

}

}
