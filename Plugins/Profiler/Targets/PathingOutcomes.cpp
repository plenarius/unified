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
#include "API/CNWSObject.hpp"

#include <algorithm>
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

// Time spent in PlotPathInArea per request (a request is time sliced over several calls).
struct RequestTime { int64_t totalNs = 0; int64_t maxNs = 0; };
static std::unordered_map<CPathfindInformation*, RequestTime> g_times;

static PathingOutcomes::LogSettings g_log;
// When each creature may be logged again.
static std::unordered_map<ObjectID, std::chrono::steady_clock::time_point> g_nextLog;

static Hooks::Hook s_PlotPathInAreaHook;
static Hooks::Hook s_GenerateInterTilePathHook;
static Hooks::Hook s_MainLoopHook;

// AI level -1 (default) to 4, stored as 0 to 5.
static uint8_t AILevelBits(int32_t aiLevel)
{
    return uint8_t(std::clamp(aiLevel + 1, 0, 7));
}

static uint64_t MakeKey(ObjectID oidArea, uint8_t aiBits, Outcome outcome, bool bPC, bool bGrid, bool bNoRoute)
{
    return (uint64_t(oidArea) << 8) | (aiBits << 5) | (outcome << 3) | (bPC << 2) | (bGrid << 1) | uint64_t(bNoRoute);
}

static void LogRequest(const CPathfindInformation* pf, CNWSObject* self, const char* what, bool bPC,
                       bool bNoRoute, const RequestTime& t)
{
    const auto now = std::chrono::steady_clock::now();
    auto& next = g_nextLog[pf->m_oidSelf];
    if (now < next)
        return;
    next = now + g_log.interval;

    std::string area = "(unknown)";
    if (CNWSArea* pArea = Globals::AppManager()->m_pServerExoApp->GetAreaByGameObjectID(pf->m_oidNewPointStartArea))
    {
        const char* resRef = pArea->m_cResRef.GetResRefStr();
        area.assign(resRef, strnlen(resRef, 16));
    }

    LOG_INFO("Pathing %s: area %s, %s '%s' (%s, %x), AI level %d, from (%.2f, %.2f) to (%.2f, %.2f), range %.2f, "
             "%d calls, %.1f ms (slowest call %.1f ms), grid search %d, no tile route %d",
             what, area.c_str(), bPC ? "PC" : "NPC",
             self ? self->m_sTag.CStr() : "", self ? self->m_sTemplate.CStr() : "", pf->m_oidSelf,
             self ? self->m_nAILevel : -1,
             pf->m_fNewPointStartX, pf->m_fNewPointStartY, pf->m_fNewPointEndX, pf->m_fNewPointEndY,
             pf->m_fPointTolerance, pf->m_nCallsToPlotPath, t.totalNs / 1e6, t.maxNs / 1e6,
             pf->m_bUsePlotGridPath ? 1 : 0, bNoRoute ? 1 : 0);
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
    {
        g_noRoute.erase(pf);
        g_times.erase(pf);
    }

    g_currentRequest = pf;
    const auto start = std::chrono::steady_clock::now();
    const uint32_t ret = s_PlotPathInAreaHook->CallOriginal<uint32_t>(pThis, pf, nTimeSlice);
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    g_currentRequest = nullptr;

    auto& t = g_times[pf];
    t.totalNs += ns;
    t.maxNs = std::max(t.maxNs, ns);

    if (ret != ACTION_COMPLETE && ret != ACTION_FAILED)
        return ret;

    const Outcome outcome = ret == ACTION_FAILED ? OUTCOME_FAILED : ClassifyComplete(pf);
    const bool bPC = pf->m_oidSelf != Constants::OBJECT_INVALID &&
        Globals::AppManager()->m_pServerExoApp->GetClientObjectByObjectId(pf->m_oidSelf);
    const bool bNoRoute = g_noRoute.erase(pf) > 0;
    CNWSObject* self = Utils::AsNWSObject(Utils::GetGameObject(pf->m_oidSelf));
    const int32_t aiLevel = self ? self->m_nAILevel : -1;

    g_counts[MakeKey(pf->m_oidNewPointStartArea, AILevelBits(aiLevel), outcome, bPC, pf->m_bUsePlotGridPath, bNoRoute)]++;

    const RequestTime time = t;
    g_times.erase(pf);

    const bool bSlow = g_log.slowMs > 0 && time.maxNs >= int64_t(g_log.slowMs) * 1000000;
    const bool bFailed = g_log.failed && outcome == OUTCOME_FAILED && aiLevel >= g_log.failedMinAILevel;
    if (bSlow || bFailed)
        LogRequest(pf, self, bFailed ? (bSlow ? "failed (slow)" : "failed") : "slow", bPC, bNoRoute, time);

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
                { "AILevel", std::to_string(int((key >> 5) & 0x7) - 1) },
                { "Outcome", s_outcomeNames[(key >> 3) & 0x3] },
                { "Controller", (key & 0x4) ? "PC" : "NPC" },
                { "GridSearch", (key & 0x2) ? "true" : "false" },
                { "NoTileRoute", (key & 0x1) ? "true" : "false" },
            });
    }

    g_counts.clear();

    // Forget creatures whose log pause is over, and requests left unfinished
    // (their creature was destroyed mid-request) once there are many.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_nextLog.begin(); it != g_nextLog.end();)
        it = it->second <= now ? g_nextLog.erase(it) : std::next(it);
    if (g_times.size() > 4096)
        g_times.clear();
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

PathingOutcomes::PathingOutcomes(Services::MetricsProxy* metrics, std::chrono::seconds interval, const LogSettings& log)
{
    g_metrics = metrics;
    g_interval = interval;
    g_log = log;
    g_nextFlush = std::chrono::steady_clock::now() + interval;

    // Earliest, so this sees the final result even if another plugin replaces the planner.
    s_PlotPathInAreaHook = Hooks::HookFunction(&CNWSModule::PlotPathInArea, &PlotPathInAreaHook, Hooks::Order::Earliest);
    s_GenerateInterTilePathHook = Hooks::HookFunction(&CNWSArea::GenerateInterTilePath, &GenerateInterTilePathHook, Hooks::Order::Earliest);
    s_MainLoopHook = Hooks::HookFunction(&CServerExoAppInternal::MainLoop, &MainLoopHook, Hooks::Order::Late);
}

}
