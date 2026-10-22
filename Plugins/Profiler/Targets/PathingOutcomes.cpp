#include "Targets/PathingOutcomes.hpp"

#include "API/CAppManager.hpp"
#include "API/CNWSArea.hpp"
#include "API/CNWSModule.hpp"
#include "API/CPathfindInformation.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/Constants.hpp"
#include "API/Functions.hpp"
#include "API/Globals.hpp"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace Profiler {

using namespace NWNXLib;
using namespace NWNXLib::API;

// CNWSObject::ACTION_* return values of the planner.
static constexpr uint32_t ACTION_COMPLETE = 2;
static constexpr uint32_t ACTION_FAILED   = 3;

// A complete path whose last waypoint is further than this beyond the requested range
// is the engine's partial straight-line fallback (or a grid path that stopped short).
static constexpr float PARTIAL_SLACK = 1.0f;

enum Outcome : uint8_t { OUTCOME_COMPLETE, OUTCOME_PARTIAL, OUTCOME_FAILED };
static const char* const s_outcomeNames[] = { "Complete", "Partial", "Failed" };

static Services::MetricsProxy* g_metrics;
static std::chrono::seconds g_interval;
static std::chrono::steady_clock::time_point g_nextFlush;

// The request inside PlotPathInArea right now, so tile route failures can be pinned on it.
static CPathfindInformation* g_currentRequest;
// Requests whose tile route search found no route at least once.
static std::unordered_set<CPathfindInformation*> g_noRoute;

// Finished requests since the last flush, keyed by area and outcome bits (see MakeKey).
static std::unordered_map<uint64_t, uint32_t> g_counts;

static Hooks::Hook s_PlotPathInAreaHook;
static Hooks::Hook s_GenerateInterTilePathHook;
static Hooks::Hook s_MainLoopHook;

static uint64_t MakeKey(ObjectID oidArea, Outcome outcome, bool bPC, bool bGrid, bool bNoRoute)
{
    return (uint64_t(oidArea) << 8) | (outcome << 3) | (bPC << 2) | (bGrid << 1) | uint64_t(bNoRoute);
}

static Outcome ClassifyComplete(const CPathfindInformation* pf)
{
    if (pf->m_nWayPoints < 2 || !pf->m_pfWayPoints)
        return OUTCOME_COMPLETE;

    const float dx = pf->m_pfWayPoints[pf->m_nWayPoints - 2] - pf->m_fNewPointEndX;
    const float dy = pf->m_pfWayPoints[pf->m_nWayPoints - 1] - pf->m_fNewPointEndY;
    const float reach = pf->m_fPointTolerance + PARTIAL_SLACK;

    return dx * dx + dy * dy > reach * reach ? OUTCOME_PARTIAL : OUTCOME_COMPLETE;
}

static uint32_t PlotPathInAreaHook(CNWSModule* pThis, CPathfindInformation* pf, uint32_t nTimeSlice)
{
    // The engine counts calls per request from zero, so this is a fresh request.
    if (pf->m_nCallsToPlotPath == 0)
        g_noRoute.erase(pf);

    g_currentRequest = pf;
    const uint32_t ret = s_PlotPathInAreaHook->CallOriginal<uint32_t>(pThis, pf, nTimeSlice);
    g_currentRequest = nullptr;

    if (ret != ACTION_COMPLETE && ret != ACTION_FAILED)
        return ret;

    const Outcome outcome = ret == ACTION_FAILED ? OUTCOME_FAILED : ClassifyComplete(pf);
    const bool bPC = pf->m_oidSelf != Constants::OBJECT_INVALID &&
        Globals::AppManager()->m_pServerExoApp->GetClientObjectByObjectId(pf->m_oidSelf);
    const bool bNoRoute = g_noRoute.erase(pf) > 0;

    g_counts[MakeKey(pf->m_oidNewPointStartArea, outcome, bPC, pf->m_bUsePlotGridPath, bNoRoute)]++;

    return ret;
}

static int32_t GenerateInterTilePathHook(CNWSArea* pThis, int32_t* nStartDepth, int32_t nTileX, int32_t nTileY, int32_t nRegion)
{
    const int32_t ret = s_GenerateInterTilePathHook->CallOriginal<int32_t>(pThis, nStartDepth, nTileX, nTileY, nRegion);

    if (ret == 0 && g_currentRequest)
        g_noRoute.insert(g_currentRequest);

    return ret;
}

static void Flush()
{
    CServerExoApp* server = Globals::AppManager()->m_pServerExoApp;

    for (const auto& [key, count] : g_counts)
    {
        const ObjectID oidArea = ObjectID(key >> 8);
        std::string areaName = "(unknown)";

        if (CNWSArea* area = server->GetAreaByGameObjectID(oidArea))
        {
            const char* resRef = area->m_cResRef.GetResRefStr();
            areaName.assign(resRef, strnlen(resRef, 16));
        }

        g_metrics->Push("PathingOutcome",
            { { "Count", std::to_string(count) } },
            {
                { "Area", std::move(areaName) },
                { "Outcome", s_outcomeNames[(key >> 3) & 0x3] },
                { "Controller", (key & 0x4) ? "PC" : "NPC" },
                { "GridSearch", (key & 0x2) ? "true" : "false" },
                { "NoTileRoute", (key & 0x1) ? "true" : "false" },
            });
    }

    g_counts.clear();
}

static int32_t MainLoopHook(CServerExoAppInternal* pThis)
{
    const auto now = std::chrono::steady_clock::now();

    if (now >= g_nextFlush)
    {
        g_nextFlush = now + g_interval;
        Flush();
    }

    return s_MainLoopHook->CallOriginal<int32_t>(pThis);
}

PathingOutcomes::PathingOutcomes(Services::MetricsProxy* metrics, std::chrono::seconds interval)
{
    g_metrics = metrics;
    g_interval = interval;
    g_nextFlush = std::chrono::steady_clock::now() + interval;

    // Earliest, so this sees the final result even if another plugin replaces the planner.
    s_PlotPathInAreaHook = Hooks::HookFunction(&CNWSModule::PlotPathInArea, &PlotPathInAreaHook, Hooks::Order::Earliest);
    s_GenerateInterTilePathHook = Hooks::HookFunction(&CNWSArea::GenerateInterTilePath, &GenerateInterTilePathHook, Hooks::Order::Earliest);
    s_MainLoopHook = Hooks::HookFunction(&CServerExoAppInternal::MainLoop, &MainLoopHook, Hooks::Order::Late);
}

}
