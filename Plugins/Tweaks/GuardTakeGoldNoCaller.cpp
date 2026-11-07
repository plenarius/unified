#include "nwnx.hpp"

#include "API/CNWSVirtualMachineCommands.hpp"
#include "API/CVirtualMachine.hpp"


namespace Tweaks {

using namespace NWNXLib;
using namespace NWNXLib::API;
using namespace NWNXLib::API::Constants;

// TakeGoldFromCreature() gets the game object of OBJECT_SELF and calls a virtual function on it without checking that
// it exists (CNWSVirtualMachineCommands::ExecuteCommandTakeGoldFromCreature, 8193.37-17). A script run on an object that
// has since been destroyed (ExecuteScript on it) crashes the server with a segfault there. This skips the call (popping
// its arguments) when OBJECT_SELF has no game object, which is what the function does anyway for a caller it cannot
// use (the module). On unless NWNX_TWEAKS_GUARD_TAKE_GOLD_NO_CALLER is false: it only ever replaces a crash.
void GuardTakeGoldNoCaller() __attribute__((constructor));
void GuardTakeGoldNoCaller()
{
    if (!Config::Get<bool>("GUARD_TAKE_GOLD_NO_CALLER", true))
        return;

    LOG_INFO("TakeGoldFromCreature() does nothing, instead of crashing the server, when OBJECT_SELF no longer exists.");

    static Hooks::Hook s_ExecuteCommandTakeGoldFromCreatureHook =
            Hooks::HookFunction(&CNWSVirtualMachineCommands::ExecuteCommandTakeGoldFromCreature,
            +[](CNWSVirtualMachineCommands* thisPtr, int32_t nCommandId, int32_t nParameters) -> int32_t
            {
                if (Utils::GetGameObject(thisPtr->m_oidObjectRunScript))
                    return s_ExecuteCommandTakeGoldFromCreatureHook->CallOriginal<int32_t>(thisPtr, nCommandId, nParameters);

                // The arguments, in the order the engine pops them: nAmount, oCreatureToTakeFrom, bDestroy.
                auto *vm = Globals::VirtualMachine();
                int32_t nAmount, bDestroy;
                ObjectID oidCreature;
                if (!vm->StackPopInteger(&nAmount) || !vm->StackPopObject(&oidCreature) || !vm->StackPopInteger(&bDestroy))
                    return VMError::StackUnderflow;
                LOG_WARNING("TakeGoldFromCreature(%d, %x) skipped: OBJECT_SELF %x no longer exists.", nAmount, oidCreature,
                            thisPtr->m_oidObjectRunScript);
                return VMError::Success;
            }, Hooks::Order::Early);
}

}
