/// @defgroup layonara Layonara
/// @brief Various Layonara specific functions
/// @{
/// @file nwnx_layo.nss

const string NWNX_Layonara = "NWNX_Layonara"; ///< @private

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

/// @brief Drop a loaded tileset from the server, so the next area that uses it loads its .set and tile
/// walkmeshes again from the resource manager.
/// @note The server keeps every tileset it loads until it shuts down, so a tileset changed under the same name
/// (and reindexed with NWNX_Util_UpdateResourceDirectory) only reaches new areas once it is dropped. Destroy
/// every area that uses it first.
/// @param sTileset The tileset's resref, e.g. "tms01".
/// @return TRUE if the tileset was dropped, FALSE if it isn't loaded or an area still uses it.
int NWNX_Layonara_DropTileSet(string sTileset);

/// @}

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

int NWNX_Layonara_DropTileSet(string sTileset)
{
    string sFunc = "DropTileSet";

    NWNXPushString(sTileset);

    NWNXCall(NWNX_Layonara, sFunc);

    return NWNXPopInt();
}
