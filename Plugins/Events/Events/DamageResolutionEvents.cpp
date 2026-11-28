#include "Events.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CNWSCombatRound.hpp"
#include "API/CNWSCombatAttackData.hpp"

#include <algorithm>

namespace Events {

using namespace NWNXLib;
using namespace NWNXLib::API;
using namespace NWNXLib::API::Constants;

static Hooks::Hook s_ResolveDamageHook;
static Hooks::Hook s_SavingThrowRollHook;
static Hooks::Hook s_HasFeatHook;

static void ResolveDamageHook(CNWSCreature*, CNWSObject*);
static uint8_t SavingThrowRollHook(CNWSCreature*, uint8_t, uint16_t, uint8_t, ObjectID, int32_t, uint16_t, int32_t);
static int32_t HasFeatHook(CNWSCreatureStats*, uint16_t);

static bool s_DeathAttackEvents;
static bool s_DefensiveRollEvents;
static bool s_EpicDodgeEvents;

// Death Attack, Defensive Roll and Epic Dodge are resolved inline in CNWSCreature::ResolveDamage,
// so we catch their saving throw / feat check while it runs.
struct RollState
{
    bool bPending = false;
    bool bSignalled = false;
    bool bSkipped = false;
    uint16_t nDC = 0;
    int32_t nSaveResult = -1;
};

struct ResolveDamageState
{
    CNWSCreature *pAttacker;
    CNWSCreature *pTarget;
    RollState deathAttack;
    RollState defensiveRoll;
    RollState epicDodge;
    int32_t nDefensiveRollUses = -1;
};
static ResolveDamageState *s_pState;

void DamageResolutionEvents() __attribute__((constructor));
void DamageResolutionEvents()
{
    static auto HookResolveDamage = []() {
        if (!s_ResolveDamageHook)
            s_ResolveDamageHook = Hooks::HookFunction(&CNWSCreature::ResolveDamage, &ResolveDamageHook, Hooks::Order::Early);
    };
    static auto HookSavingThrowRoll = []() {
        if (!s_SavingThrowRollHook)
            s_SavingThrowRollHook = Hooks::HookFunction(&CNWSCreature::SavingThrowRoll, &SavingThrowRollHook, Hooks::Order::Earliest);
    };

    InitOnFirstSubscribe("NWNX_ON_DEATH_ATTACK_.*", []() {
        s_DeathAttackEvents = true;
        HookResolveDamage();
        HookSavingThrowRoll();
    });
    InitOnFirstSubscribe("NWNX_ON_DEFENSIVE_ROLL_.*", []() {
        s_DefensiveRollEvents = true;
        HookResolveDamage();
        HookSavingThrowRoll();
    });
    InitOnFirstSubscribe("NWNX_ON_EPIC_DODGE_.*", []() {
        s_EpicDodgeEvents = true;
        HookResolveDamage();
        s_HasFeatHook = Hooks::HookFunction(&CNWSCreatureStats::HasFeat, &HasFeatHook, Hooks::Order::Early);
    });
}

static bool SignalBefore(const std::string& event, ObjectID oidSelf, const std::string& otherTag, ObjectID oidOther, RollState& roll)
{
    PushEventData(otherTag, Utils::ObjectIDToString(oidOther));
    PushEventData("DC", std::to_string(roll.nDC));
    std::string result;
    roll.bSignalled = true;
    roll.bSkipped = !SignalEvent(event, oidSelf, &result);
    if (auto nDC = String::FromString<int32_t>(result))
        roll.nDC = std::clamp(*nDC, 0, 0xFFFF);
    return !roll.bSkipped;
}

static void SignalAfter(const std::string& event, ObjectID oidSelf, const std::string& otherTag, ObjectID oidOther, const RollState& roll)
{
    PushEventData(otherTag, Utils::ObjectIDToString(oidOther));
    PushEventData("DC", std::to_string(roll.nDC));
    PushEventData("SAVE_RESULT", std::to_string(roll.nSaveResult));
    PushEventData("BEFORE_SKIPPED", std::to_string(roll.bSkipped));
    SignalEvent(event, oidSelf);
}

void ResolveDamageHook(CNWSCreature *pThis, CNWSObject *pTarget)
{
    auto *pTargetCreature = Utils::AsNWSCreature(pTarget);
    if (!pTargetCreature)
        return s_ResolveDamageHook->CallOriginal<void>(pThis, pTarget);

    auto *pCombatRound = pThis->m_pcCombatRound;
    auto *pAttackData = pCombatRound->GetAttack(pCombatRound->m_nCurrentAttack);

    ResolveDamageState state;
    state.pAttacker = pThis;
    state.pTarget = pTargetCreature;
    // Same conditions the game checks before rolling the paralysis save.
    state.deathAttack.bPending = s_DeathAttackEvents && pAttackData->m_bDeathAttack == 1 &&
                                 pCombatRound->m_nCurrentAttack == 0 && !pTargetCreature->m_bCombatState;
    state.defensiveRoll.bPending = s_DefensiveRollEvents;
    state.epicDodge.bPending = s_EpicDodgeEvents && pTargetCreature->m_pcCombatRound &&
                               !pTargetCreature->m_pcCombatRound->m_bEpicDodgeUsed;

    auto *pPrevState = s_pState;
    s_pState = &state;
    s_ResolveDamageHook->CallOriginal<void>(pThis, pTarget);
    s_pState = pPrevState;

    // A skipped Defensive Roll doesn't spend a use.
    if (state.nDefensiveRollUses >= 0)
        pTargetCreature->m_pStats->SetFeatRemainingUses(Feat::DefensiveRoll, state.nDefensiveRollUses);

    if (state.defensiveRoll.bSignalled)
        SignalAfter("NWNX_ON_DEFENSIVE_ROLL_AFTER", pTargetCreature->m_idSelf, "ATTACKER_OBJECT_ID", pThis->m_idSelf, state.defensiveRoll);
    if (state.deathAttack.bSignalled)
        SignalAfter("NWNX_ON_DEATH_ATTACK_AFTER", pThis->m_idSelf, "TARGET_OBJECT_ID", pTargetCreature->m_idSelf, state.deathAttack);
    if (state.epicDodge.bSignalled)
    {
        PushEventData("ATTACKER_OBJECT_ID", Utils::ObjectIDToString(pThis->m_idSelf));
        PushEventData("BEFORE_SKIPPED", std::to_string(state.epicDodge.bSkipped));
        SignalEvent("NWNX_ON_EPIC_DODGE_AFTER", pTargetCreature->m_idSelf);
    }
}

uint8_t SavingThrowRollHook(CNWSCreature *pThis, uint8_t nSaveType, uint16_t nDC, uint8_t nSpecificType,
                            ObjectID oidSaveVersus, int32_t bPrint, uint16_t nFeat, int32_t bQueueFeedback)
{
    auto *pState = s_pState;
    if (!pState || pThis != pState->pTarget || oidSaveVersus != pState->pAttacker->m_idSelf)
        return s_SavingThrowRollHook->CallOriginal<uint8_t>(pThis, nSaveType, nDC, nSpecificType, oidSaveVersus, bPrint, nFeat, bQueueFeedback);

    if (pState->deathAttack.bPending && nSaveType == SavingThrow::Fortitude &&
        nSpecificType == SavingThrowType::Paralysis && nFeat == 0)
    {
        auto& roll = pState->deathAttack;
        roll.bPending = false;
        roll.nDC = nDC;
        // A successful save is the only way to stop the game applying the paralysis.
        if (!SignalBefore("NWNX_ON_DEATH_ATTACK_BEFORE", pState->pAttacker->m_idSelf, "TARGET_OBJECT_ID", pThis->m_idSelf, roll))
            return 1;
        roll.nSaveResult = s_SavingThrowRollHook->CallOriginal<uint8_t>(pThis, nSaveType, roll.nDC, nSpecificType, oidSaveVersus, bPrint, nFeat, bQueueFeedback);
        return roll.nSaveResult;
    }

    if (pState->defensiveRoll.bPending && nSaveType == SavingThrow::Reflex && nFeat == Feat::DefensiveRoll)
    {
        auto& roll = pState->defensiveRoll;
        roll.bPending = false;
        roll.nDC = nDC;
        if (!SignalBefore("NWNX_ON_DEFENSIVE_ROLL_BEFORE", pThis->m_idSelf, "ATTACKER_OBJECT_ID", pState->pAttacker->m_idSelf, roll))
        {
            // The game spends the use after this returns, ResolveDamageHook gives it back.
            pState->nDefensiveRollUses = pThis->m_pStats->GetFeatRemainingUses(Feat::DefensiveRoll);
            return 0;
        }
        roll.nSaveResult = s_SavingThrowRollHook->CallOriginal<uint8_t>(pThis, nSaveType, roll.nDC, nSpecificType, oidSaveVersus, bPrint, nFeat, bQueueFeedback);
        return roll.nSaveResult;
    }

    return s_SavingThrowRollHook->CallOriginal<uint8_t>(pThis, nSaveType, nDC, nSpecificType, oidSaveVersus, bPrint, nFeat, bQueueFeedback);
}

int32_t HasFeatHook(CNWSCreatureStats *pThis, uint16_t nFeat)
{
    auto retVal = s_HasFeatHook->CallOriginal<int32_t>(pThis, nFeat);

    auto *pState = s_pState;
    if (retVal && nFeat == Feat::EpicDodge && pState && pState->epicDodge.bPending && pThis == pState->pTarget->m_pStats)
    {
        // Only asked once the hit deals damage, so this is the dodge about to happen.
        auto& roll = pState->epicDodge;
        roll.bPending = false;
        roll.bSignalled = true;
        PushEventData("ATTACKER_OBJECT_ID", Utils::ObjectIDToString(pState->pAttacker->m_idSelf));
        roll.bSkipped = !SignalEvent("NWNX_ON_EPIC_DODGE_BEFORE", pState->pTarget->m_idSelf);
        if (roll.bSkipped)
            return false;
    }

    return retVal;
}

}
