#include "nwnx.hpp"

#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSMessage.hpp"
#include "API/CNWCCMessageData.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CTlkTable.hpp"

#include <cstdlib>

// The skill check feedback line ("<name> : Open Lock : *success* : (20 + 73 = 93 vs. DC: 200)")
// is a UseSkill CC message, and the client reads its DC in 7 bits and the skill modifier as a
// signed 8 bit value. A lock or trap with a DC over 127 shows the DC modulo 128 (DC 200 shows
// as DC 72), and a skill modifier over 127 wraps negative. The check itself uses the real values.
//
// With this tweak, a UseSkill message whose DC or modifier doesn't fit is sent as feedback text
// instead, built the way the client builds the line (same TLK entries, same name colours).
// Everything else goes through unchanged. It hooks after the Feedback plugin, so a player who
// has skill feedback hidden still doesn't get it.

namespace Tweaks {

using namespace NWNXLib;
using namespace NWNXLib::API;

static Hooks::Hook s_SendServerToPlayerCCMessageHook;

static int32_t SendServerToPlayerCCMessageHook(CNWSMessage*, uint32_t, uint8_t, CNWCCMessageData*, CNWSCombatAttackData*);

void FixSkillCheckDisplay() __attribute__((constructor));
void FixSkillCheckDisplay()
{
    if (!Config::Get<bool>("FIX_SKILL_CHECK_DISPLAY", false))
        return;

    LOG_INFO("Skill checks whose DC or modifier the client can't display are sent as feedback text.");

    s_SendServerToPlayerCCMessageHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerCCMessage,
                                                            &SendServerToPlayerCCMessageHook, Hooks::Order::VeryLate);
}

static std::string TlkString(uint32_t nStrRef)
{
    CExoString s = Globals::TlkTable()->GetSimpleString(nStrRef);
    return s.CStr() ? s.CStr() : "";
}

static void ReplaceToken(std::string& sText, const std::string& sToken, const std::string& sValue)
{
    for (size_t pos = sText.find(sToken); pos != std::string::npos; pos = sText.find(sToken, pos + sValue.size()))
        sText.replace(pos, sToken.size(), sValue);
}

static int32_t SendServerToPlayerCCMessageHook(CNWSMessage *pMessage, uint32_t nPlayerId, uint8_t nMinor,
                                              CNWCCMessageData *pData, CNWSCombatAttackData *pAttackData)
{
    auto CallOriginal = [&]() {
        return s_SendServerToPlayerCCMessageHook->CallOriginal<int32_t>(pMessage, nPlayerId, nMinor, pData, pAttackData);
    };

    if (nMinor != Constants::MessageClientSideMsgMinor::UseSkill || !pData)
        return CallOriginal();

    // Message data: object 0 the skill user; integers 0 skill name strref, 1 d20, 2 modifier,
    // 3 DC, 4 took 20, 5 result.
    const int32_t nRoll = pData->GetInteger(1);
    const int32_t nModifier = pData->GetInteger(2);
    const int32_t nDC = pData->GetInteger(3);
    if (nDC >= 0 && nDC <= 127 && nModifier >= -128 && nModifier <= 127)
        return CallOriginal();

    auto *pServer = Globals::AppManager()->m_pServerExoApp;
    auto *pPlayer = pServer->GetClientObjectByPlayerId(nPlayerId);
    auto *pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pData->GetObjectID(0)));
    if (!pPlayer || !pCreature || !pCreature->m_pStats)
        return CallOriginal();

    // The name, coloured as the client does: light blue (153, 255, 255) for yourself, pink
    // (204, 153, 204) for others.
    std::string sName = pCreature->m_sDisplayName.CStr() && *pCreature->m_sDisplayName.CStr() ?
                        pCreature->m_sDisplayName.CStr() : pCreature->m_pStats->GetFullName().CStr();
    auto *pPlayerObject = pPlayer->GetGameObject();
    const bool bSelf = pPlayerObject && pPlayerObject->m_idSelf == pCreature->m_idSelf;
    const int r = bSelf ? 153 : 204, g = bSelf ? 255 : 153, b = bSelf ? 255 : 204;
    sName = CExoString::F("<c%c%c%c>", r, g, b).CStr() + sName + "</c>";

    static const std::unordered_map<int32_t, uint32_t> s_results = {
        {0, 5353},  // failure
        {1, 5352},  // success
        {2, 8100},  // critical failure
        {3, 8101},  // success not possible
        {4, 53306}, // automatic success
        {5, 40091}, // Success will never be possible
    };
    const int32_t nResult = pData->GetInteger(5);
    const auto result = s_results.find(nResult);
    const std::string sResult = result != s_results.end() ? TlkString(result->second) : "";
    const std::string sSkill = TlkString(pData->GetInteger(0));

    std::string sText;
    if (nResult == 5)
    {
        sText = sName + " : " + sSkill + " : *" + sResult + "*";
    }
    else
    {
        // "<CUSTOM0> : <CUSTOM1> : *<CUSTOM2>* : (<CUSTOM3> <CUSTOM4> <CUSTOM5> = <CUSTOM6> vs. DC: <CUSTOM7>)"
        sText = TlkString(10484);
        ReplaceToken(sText, "<CUSTOM0>", sName);
        ReplaceToken(sText, "<CUSTOM1>", sSkill);
        ReplaceToken(sText, "<CUSTOM2>", sResult);
        ReplaceToken(sText, "<CUSTOM3>", std::to_string(nRoll));
        ReplaceToken(sText, "<CUSTOM4>", nModifier < 0 ? "-" : "+");
        ReplaceToken(sText, "<CUSTOM5>", std::to_string(std::abs(nModifier)));
        ReplaceToken(sText, "<CUSTOM6>", std::to_string(nRoll + nModifier));
        ReplaceToken(sText, "<CUSTOM7>", std::to_string(nDC));
        if (pData->GetInteger(4))
            sText += " : " + TlkString(10485); // Take 20
    }

    LOG_DEBUG("Skill check sent as text to player %u: %s", nPlayerId, sText);
    pCreature->SendFeedbackString(CExoString(sText), pPlayer);
    return true;
}

}
