// TEMPORARY measurement code for nwnxee/unified#1537 (event queue). Not for merging.
// NWNX_OPTIMIZATIONS_EVENT_QUEUE_BENCH=true: times every CServerAIMaster::AddEventDeltaTime and UpdateState,
// samples the queue length every frame and logs a summary every NWNX_OPTIMIZATIONS_EVENT_QUEUE_BENCH_PERIOD seconds.
#include "nwnx.hpp"

#include "API/CServerAIMaster.hpp"
#include "API/CExoLinkedListInternal.hpp"

#include <chrono>
#include <time.h>

namespace Optimizations
{

using namespace NWNXLib;
using namespace NWNXLib::API;

static bool s_PriorityQueue;
static bool EventMasterPriorityQueueEnabled() { return s_PriorityQueue; }

static Hooks::Hook s_AddEventDeltaTimeHook;
static Hooks::Hook s_UpdateStateHook;
static CServerAIMaster *s_pAIMaster;

struct Stats
{
    uint64_t adds = 0, addNs = 0, addMaxNs = 0;
    uint64_t frames = 0, updNs = 0, updMaxNs = 0;
    uint64_t qSum = 0, qMax = 0, qMin = UINT64_MAX;
};
static Stats s_Period, s_Script;
static uint64_t s_PeriodStart, s_PeriodNs;

static inline uint64_t Now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static size_t QueueSize()
{
    // Under EVENT_MASTER_PRIORITY_QUEUE the list's count follows the heap.
    return s_pAIMaster ? s_pAIMaster->m_lEventQueue.m_pcExoLinkedListInternal->m_nCount : 0;
}

static BOOL AddEventDeltaTime(CServerAIMaster *pThis, uint32_t nDays, uint32_t nTime, OBJECT_ID nCaller, OBJECT_ID nObject, uint32_t nEventId, void *pData)
{
    s_pAIMaster = pThis;
    uint64_t t0 = Now();
    auto ret = s_AddEventDeltaTimeHook->CallOriginal<BOOL>(pThis, nDays, nTime, nCaller, nObject, nEventId, pData);
    uint64_t dt = Now() - t0;
    for (Stats *s : {&s_Period, &s_Script})
    {
        s->adds++; s->addNs += dt;
        if (dt > s->addMaxNs) s->addMaxNs = dt;
    }
    return ret;
}

static void UpdateState(CServerAIMaster *pThis)
{
    s_pAIMaster = pThis;
    uint64_t q = QueueSize();
    uint64_t t0 = Now();
    s_UpdateStateHook->CallOriginal<void>(pThis);
    uint64_t t1 = Now();
    uint64_t dt = t1 - t0;
    for (Stats *s : {&s_Period, &s_Script})
    {
        s->frames++; s->updNs += dt;
        if (dt > s->updMaxNs) s->updMaxNs = dt;
        s->qSum += q;
        if (q > s->qMax) s->qMax = q;
        if (q < s->qMin) s->qMin = q;
    }

    if (!s_PeriodStart) s_PeriodStart = t1;
    if (t1 - s_PeriodStart >= s_PeriodNs)
    {
        const Stats& s = s_Period;
        LOG_INFO("EVQ %s frames=%llu q_min=%llu q_avg=%llu q_max=%llu adds=%llu add_avg_ns=%llu add_max_us=%llu upd_avg_us=%llu upd_max_us=%llu",
                 EventMasterPriorityQueueEnabled() ? "B" : "A",
                 s.frames, s.frames ? s.qMin : 0, s.frames ? s.qSum / s.frames : 0, s.qMax,
                 s.adds, s.adds ? s.addNs / s.adds : 0, s.addMaxNs / 1000,
                 s.frames ? s.updNs / s.frames / 1000 : 0, s.updMaxNs / 1000);
        s_Period = {};
        s_PeriodStart = t1;
    }
}

void EventQueueBench() __attribute__((constructor));
void EventQueueBench()
{
    if (!Config::Get<bool>("EVENT_QUEUE_BENCH", false))
        return;
    s_PriorityQueue = Config::Get<bool>("EVENT_MASTER_PRIORITY_QUEUE", false);
    s_PeriodNs = uint64_t(Config::Get<int>("EVENT_QUEUE_BENCH_PERIOD", 10)) * 1000000000ull;
    s_AddEventDeltaTimeHook = Hooks::HookFunction(&CServerAIMaster::AddEventDeltaTime, &AddEventDeltaTime, Hooks::Order::Earliest);
    s_UpdateStateHook = Hooks::HookFunction(&CServerAIMaster::UpdateState, &UpdateState, Hooks::Order::Earliest);
}

// No nwscript wrapper: NWNXCall("NWNX_Optimizations", "EvqBenchReset") / "EvqBenchGet" (returns a string).
extern "C" ArgumentStack EvqBenchReset(ArgumentStack&&)
{
    s_Script = {};
    return {};
}

extern "C" ArgumentStack EvqBenchGet(ArgumentStack&&)
{
    const Stats& s = s_Script;
    std::string r = std::string(EventMasterPriorityQueueEnabled() ? "B" : "A") +
        " q_now=" + std::to_string(QueueSize()) +
        " q_max=" + std::to_string(s.qMax) +
        " q_avg=" + std::to_string(s.frames ? s.qSum / s.frames : 0) +
        " adds=" + std::to_string(s.adds) +
        " add_total_us=" + std::to_string(s.addNs / 1000) +
        " add_avg_ns=" + std::to_string(s.adds ? s.addNs / s.adds : 0) +
        " add_max_us=" + std::to_string(s.addMaxNs / 1000) +
        " frames=" + std::to_string(s.frames) +
        " upd_total_us=" + std::to_string(s.updNs / 1000) +
        " upd_max_us=" + std::to_string(s.updMaxNs / 1000);
    return r;
}

}
