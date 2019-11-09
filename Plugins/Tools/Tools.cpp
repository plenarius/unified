#include "Tools.hpp"

#include "API/CExoBase.hpp"
#include "API/CNWSArea.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSDoor.hpp"
#include "API/CNWSModule.hpp"
#include "API/CNWSPlaceable.hpp"
#include "API/CNWSScriptVar.hpp"
#include "API/CNWSTile.hpp"
#include "API/CNWSTrigger.hpp"
#include "API/CNWSWaypoint.hpp"
#include "API/CNWTileData.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "ViewPtr.hpp"
#include <Magick++.h>
#include <fstream>
#include <experimental/filesystem>


using namespace NWNXLib;
using namespace NWNXLib::API;
namespace fs = std::experimental::filesystem;
static ViewPtr<Tools::Tools> g_plugin;

NWNX_PLUGIN_ENTRY Plugin::Info* PluginInfo()
{
    return new Plugin::Info
    {
        "Tools",
        "NWN tools that are outside the regular scope of running the server.",
        "orth",
        "plenarius@gmail.com",
        1,
        true
    };
}

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Plugin::CreateParams params)
{
    g_plugin = new Tools::Tools(params);
    return g_plugin;
}


namespace Tools {

Tools::Tools(const Plugin::CreateParams& params)
    : Plugin(params)
{
#define REGISTER(func) \
    GetServices()->m_events->RegisterEvent(#func, \
        [this](ArgumentStack&& args){ return func(std::move(args)); })

    REGISTER(GenerateMiniMaps);
    REGISTER(GenerateAreaGraph);

#undef REGISTER
}

Tools::~Tools()
{
}

ArgumentStack Tools::GenerateMiniMaps(ArgumentStack && args)
{
    ArgumentStack stack;
    const auto sMinimapPath = Services::Events::ExtractArgument<std::string>(args);
    if (!std::experimental::filesystem::is_directory(sMinimapPath))
    {
        LOG_ERROR("Minimap directory path \"%s\" does not exist.", sMinimapPath.c_str());
        return stack;
    }
    const auto bOverwriteExisting = Services::Events::ExtractArgument<int32_t>(args);
    const auto sExtension = Services::Events::ExtractArgument<std::string>(args);
    const auto nImageSize = Services::Events::ExtractArgument<int32_t>(args);
    const auto bUseTagFilename = Services::Events::ExtractArgument<int32_t>(args);

    auto pModule = Utils::GetModule();
    auto areaList = pModule->m_lstModuleAreaID;
    for (int i = 0; i < areaList.num; i++)
    {
        auto *pArea = Utils::GetGameObject(areaList.element[i])->AsNWSArea();

        std::list<Magick::Image> sourceImageList;
        Magick::Image image;
        std::string montageName = pArea->m_cResRef.GetResRefStr();
        if (bUseTagFilename)
            montageName.append(pArea->m_sTag.CStr());

        montageName.append("." + sExtension);
        std::transform(montageName.begin(), montageName.end(), montageName.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        fs::path userDir(API::Globals::ExoBase()->m_sUserDirectory.CStr());
        fs::path minimapDir("minimaps");
        if (!fs::is_directory(userDir / minimapDir))
            fs::create_directory(userDir / minimapDir);
        fs::path full_path = userDir / minimapDir / montageName;
        if (!bOverwriteExisting && fs::exists(full_path))
            continue;
        auto areaWidth = pArea->m_nWidth;
        auto areaHeight = pArea->m_nHeight;
        std::string montageTile = std::to_string(areaWidth) + "x";
        std::string montageGeometry = std::to_string(nImageSize) + "x" + std::to_string(nImageSize) + "-0-0";
        for (int32_t y = areaHeight - 1; y >= 0; y--)
        {
            for (int32_t x = 0; x < areaWidth; x++)
            {
                int32_t nTile = y * areaWidth + x;
                CNWSTile *pTile = &pArea->m_pTile[nTile];
                if (pTile != nullptr)
                {
                    auto tileData = pTile->GetTileData();
                    auto fpp = pTile->m_nOrientation * -90.0f;
                    std::string minimap = tileData->GetMapIcon().GetResRefStr();
                    std::transform(minimap.begin(), minimap.end(), minimap.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    std::string mmfn = sMinimapPath + "/";
                    mmfn.append(minimap);
                    mmfn.append(".tga");
                    if (std::experimental::filesystem::exists(mmfn))
                    {
                        image.read(mmfn);
                        image.rotate(fpp);
                        sourceImageList.emplace_back(image);
                    }
                    else
                    {
                        mmfn = sMinimapPath;
                        mmfn.append("/black.tga");
                        if (std::experimental::filesystem::exists(mmfn))
                        {
                            LOG_WARNING("Missing minimap image %s", tileData->GetMapIcon().GetResRefStr());
                            image.read(mmfn);
                            sourceImageList.push_back(image);
                        }
                        else
                        {
                            LOG_ERROR("No missing minimap image was found, include a black.tga in your minimap folder.");
                            return stack;
                        }
                    }
                }
            }
        }

        Magick::Color color("rgba(0,0,0,0)");

        Magick::Montage montageSettings;
        montageSettings.geometry(montageGeometry.c_str());
        montageSettings.shadow(false);
        montageSettings.backgroundColor(color);
        montageSettings.tile(montageTile.c_str());

        std::list<Magick::Image> montagelist;
        Magick::montageImages(&montagelist, sourceImageList.begin(), sourceImageList.end(), montageSettings);
        Magick::writeImages(montagelist.begin(), montagelist.end(), full_path);
    }

    return stack;
}

ArgumentStack Tools::GenerateAreaGraph(ArgumentStack && args)
{
    ArgumentStack stack;
    const auto sMinimapPath = Services::Events::ExtractArgument<std::string>(args);
    auto sOutputFile = Services::Events::ExtractArgument<std::string>(args);
    const auto sExtension = Services::Events::ExtractArgument<std::string>(args);
    const auto bTagFilename = Services::Events::ExtractArgument<int32_t>(args);
    const auto nLabelType = Services::Events::ExtractArgument<int32_t>(args);

    auto pModule = Utils::GetModule();

    std::string sModuleName = Utils::ExtractLocString(pModule->m_lsModuleName);
    if (sOutputFile.empty())
    {
        sOutputFile = sModuleName + ".dot";
    }
    fs::path userDir(API::Globals::ExoBase()->m_sUserDirectory.CStr());
    fs::path graphDir("graphs");
    if (!fs::is_directory(userDir / graphDir))
        fs::create_directory(userDir / graphDir);
    fs::path full_path = userDir / graphDir / sOutputFile;

    std::stringstream dotText;
    dotText << "strict digraph area_maps {\n   rankdir=LR\n   concentrate=true\n    ranksep = \"0.6\"\n";

    auto areaList = pModule->m_lstModuleAreaID;
    for (int i = 0; i < areaList.num; i++)
    {
        auto *pArea = Utils::GetGameObject(areaList.element[i])->AsNWSArea();

        auto sAreaTag = pArea->m_sTag;

        std::string sNodeLabel;
        switch(nLabelType)
        {
            case 0:
                sNodeLabel = pArea->m_cResRef.GetResRefStr();
                break;
            case 1:
                sNodeLabel = pArea->m_sTag.CStr();
                break;
            case 2:
            {
                CExoString sName;
                CExoLocString *lsName = &pArea->m_lsName;
                lsName->GetString(0, &sName, 0, true);
                sNodeLabel = sName.CStr();
            }
        }

        if (!sMinimapPath.empty())
        {
            std::string areaMiniMapFile = sMinimapPath + "/";
            if (bTagFilename)
                areaMiniMapFile.append(pArea->m_sTag.CStr());
            else
                areaMiniMapFile.append(pArea->m_cResRef.GetResRefStr());
            areaMiniMapFile.append("." + sExtension);

            dotText << "        " << sAreaTag.CStr() << "[shape=none, labelloc=t, label=<\n"
                                                        "    <table bgcolor=\"white\" border=\"1\" cellborder=\"0\" cellspacing=\"3\">\n"
                                                        "        <tr>\n"
                                                        "            <td>" << sNodeLabel << "</td>\n"
                                                        "        </tr>\n"
                                                        "    </table>>, image=\"" << areaMiniMapFile.c_str() << "\"];\n";
        }
        else
        {
            dotText << "        " << sAreaTag.CStr() << "[shape=ellipse, labelloc=T, label=\"" << sNodeLabel << "\"];\n";
        }
        uint32_t oid;
        // Find all our transitions and build our pairs
        if (pArea->GetFirstObjectInArea(oid))
        {
            while (oid != Constants::OBJECT_INVALID)
            {
                auto *pGameObject = Utils::GetGameObject(oid);
                switch (pGameObject->m_nObjectType)
                {
                    case Constants::ObjectType::Trigger:
                    {
                        auto *pTrigger = pGameObject->AsNWSTrigger();
                        if (pTrigger != nullptr)
                        {
                            auto *pTarget = pTrigger->m_pTransition.LookupTarget();
                            if (pTarget != nullptr && pTarget->m_oidArea != pArea->m_idSelf)
                            {
                                auto *pTargetArea = Utils::GetGameObject(pTarget->m_oidArea)->AsNWSArea();
                                dotText << sAreaTag.CStr() << " -> " << pTargetArea->m_sTag.CStr()
                                        << " [style=dashed color=\"blue\"];\n";
                            }
                        }
                        break;
                    }
                    case Constants::ObjectType::Door:
                    {
                        auto *pDoor = pGameObject->AsNWSDoor();
                        if (pDoor != nullptr)
                        {
                            auto *pTarget = pDoor->m_pTransition.LookupTarget();
                            if (pTarget != nullptr && pTarget->m_oidArea != pArea->m_idSelf)
                            {
                                auto *pTargetArea = Utils::GetGameObject(pTarget->m_oidArea)->AsNWSArea();
                                dotText << sAreaTag.CStr() << " -> " << pTargetArea->m_sTag.CStr()
                                        << " [style=solid, arrowhead=box, color=\"burlywood4\"];\n";
                            }
                        }
                        break;
                    }
                    default:
                    {
                        // Search for any variables on this object that are waypoints
                        auto *pVarTable = Utils::GetScriptVarTable(pGameObject);
                        for (int32_t index = 0; index < pVarTable->m_lVarList.num; index++)
                        {
                            if (pVarTable->m_lVarList.element[index].m_nType == 3) // String Var
                            {
                                auto sValue = pVarTable->GetString(pVarTable->m_lVarList.element[index].m_sName);
                                auto pPortalWPOid = pModule->FindObjectByTagTypeOrdinal(sValue, Constants::ObjectType::Waypoint, 0);
                                int32_t j = 0;
                                while (pPortalWPOid != Constants::OBJECT_INVALID)
                                {
                                    auto *pPortalWP = Utils::GetGameObject(pPortalWPOid)->AsNWSWaypoint();
                                    if (pPortalWP && pPortalWP->m_oidArea != pArea->m_idSelf)
                                    {
                                        auto *pTargetArea = Utils::GetGameObject(pPortalWP->m_oidArea)->AsNWSArea();
                                        if (pTargetArea != nullptr)
                                        {
                                            dotText << sAreaTag.CStr() << " -> " << pTargetArea->m_sTag.CStr()
                                                    << " [style=dotted, arrowhead=diamond, color=\"red\"];\n";
                                        }
                                    }
                                    j++;
                                    pPortalWPOid = pModule->FindObjectByTagTypeOrdinal(sValue, Constants::ObjectType::Waypoint, j);
                                }
                            }
                        }
                    }
                }
                if (!pArea->GetNextObjectInArea(oid))
                    break;
            }
        }
    }
    dotText << "}";
    std::ofstream outFile(full_path);
    outFile << dotText.rdbuf();
    outFile.close();

    return stack;
}
}
