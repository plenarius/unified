/// @defgroup layonara Layonara
/// @brief Various Layonara specific functions
/// @{
/// @file nwnx_layo.nss

const string NWNX_Layonara = "NWNX_Layonara"; ///< @private

/// @brief Set the slots you can equip a base item
/// @param nBaseItem The base item id.
/// @param nSlots The slots this item can be equipped.
void NWNX_Layonara_SetEquippableSlots(int nBaseItem, int nSlots);

/// @brief Sets a feat as hostile
/// @param nFeatID The feat id.
/// @param bHostile TRUE if hostile.
void NWNX_Layonara_SetHostileFeat(int nFeatID, int bHostile);

/// @brief Enable the Duelist Canny Defense effects
/// @param creature The creature object.
/// @param nBonus The bonus amount.
void NWNX_Layonara_SetDuelistCannyDefense(object creature, int nBonus);

/// @brief Enable the Duelist Grace effects.
/// @param creature The creature object.
/// @param nBonus The bonus amount.
void NWNX_Layonara_SetDuelistGrace(object creature, int nBonus);

/// @brief Enable the Duelist Elaborate Parry effects.
/// @param creature The creature object.
/// @param nBonus The bonus amount.
void NWNX_Layonara_SetDuelistElaborateParry(object creature, int nBonus);

/// @brief Set the Subrace Effects for Daytime
/// @param creature The creature object.
/// @param nActive TRUE if turned on.
void NWNX_Layonara_SetSubraceDayEffects(object creature, int nActive = TRUE);

/// @brief Apply a Rune
/// @param creature The creature object.
/// @param item The rune
void NWNX_Layonara_ApplyRune(object creature, object item);

/// @brief Combine runes
/// @param target The target rune
/// @param item The original rune
/// @return The resref of the new rune.
string NWNX_Layonara_CombineRunes(object target, object item);

/// @brief Get rune description
/// @param item The rune.
/// @return A description of a rune's properties.
string NWNX_Layonara_GetRuneDescription(object item);

/// @brief Apply a quiver effect
/// @param creature The creature object.
/// @param nColor The quiver color.
void NWNX_Layonara_SetQuiver(object creature, int nColor);

/// @brief Apply quiver arrow effects
/// @param creature The creature object.
void NWNX_Layonara_SetQuiverArrows(object creature);

/// @brief Place new AT VFX at centroid of transitions
void NWNX_Layonara_CreateVFXAtTransitionCentroid();

/// @brief Clear the surface material so a change is fired on next movement.
/// @param creature The creature object.
void NWNX_Layonara_ClearSurfaceMaterial(object creature);

/// @brief Get the NWSync manifest hash the server advertises to connecting clients.
/// @return The 40 character lowercase SHA1, or "" if the server advertises no manifest.
string NWNX_Layonara_GetNWSyncAdvertHash();

/// @brief Get the NWSync URL the server advertises to connecting clients.
/// @return The URL, or "" if the server advertises no NWSync repository.
string NWNX_Layonara_GetNWSyncAdvertUrl();

/// @brief Change the NWSync manifest the server advertises, at runtime and without a restart.
/// @note Every client that connects afterwards gets the new manifest; players already in the game keep what
/// they downloaded. The manifest must already be published at sUrl and hold every hak and the TLK the module
/// uses, or connecting clients fail their sync. Any additional manifests the server advertised are dropped.
/// @param sHash The primary manifest's SHA1: 40 hex characters.
/// @param sUrl The NWSync repository URL, as for -nwsyncurl.
/// @remark Pass "" for both to stop advertising NWSync, as if the server had started without it.
/// @return TRUE if the advert changed, FALSE if sHash isn't a SHA1 or only one of the two is empty.
int NWNX_Layonara_SetNWSyncAdvert(string sHash, string sUrl);

/// @}

void NWNX_Layonara_SetEquippableSlots(int nBaseItem, int nSlots)
{
    string sFunc = "SetEquippableSlots";

    NWNXPushInt(nSlots);
    NWNXPushInt(nBaseItem);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetHostileFeat(int nFeatID, int bHostile)
{
    string sFunc = "SetHostileFeat";

    NWNXPushInt(bHostile);
    NWNXPushInt(nFeatID);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetDuelistCannyDefense(object creature, int nBonus)
{
    string sFunc = "SetDuelistCannyDefense";

    NWNXPushInt(nBonus);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetDuelistGrace(object creature, int nBonus)
{
    string sFunc = "SetDuelistGrace";

    NWNXPushInt(nBonus);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetDuelistElaborateParry(object creature, int nBonus)
{
    string sFunc = "SetDuelistElaborateParry";

    NWNXPushInt(nBonus);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetSubraceDayEffects(object creature, int nActive = TRUE)
{
    string sFunc = "SetSubraceDayEffects";

    NWNXPushInt(nActive);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_ApplyRune(object creature, object item)
{
    string sFunc = "ApplyRune";

    NWNXPushObject(item);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

string NWNX_Layonara_CombineRunes(object target, object item)
{
    string sFunc = "CombineRunes";

    NWNXPushObject(item);
    NWNXPushObject(target);

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopString();
}

string NWNX_Layonara_GetRuneDescription(object item)
{
    string sFunc = "GetRuneDescription";

    NWNXPushObject(item);

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopString();
}

void NWNX_Layonara_SetQuiver(object creature, int nColor)
{
    string sFunc = "SetQuiver";

    NWNXPushInt(nColor);
    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_SetQuiverArrows(object creature)
{
    string sFunc = "SetQuiverArrows";

    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_CreateVFXAtTransitionCentroid()
{
    string sFunc = "CreateVFXAtTransitionCentroid";

    NWNXCall(NWNX_Layonara, sFunc);
}

void NWNX_Layonara_ClearSurfaceMaterial(object creature)
{
    string sFunc = "ClearSurfaceMaterial";

    NWNXPushObject(creature);

    NWNXCall(NWNX_Layonara, sFunc);
}

string NWNX_Layonara_GetNWSyncAdvertHash()
{
    string sFunc = "GetNWSyncAdvertHash";

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopString();
}

string NWNX_Layonara_GetNWSyncAdvertUrl()
{
    string sFunc = "GetNWSyncAdvertUrl";

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopString();
}

int NWNX_Layonara_SetNWSyncAdvert(string sHash, string sUrl)
{
    string sFunc = "SetNWSyncAdvert";

    NWNXPushString(sUrl);
    NWNXPushString(sHash);

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopInt();
}
