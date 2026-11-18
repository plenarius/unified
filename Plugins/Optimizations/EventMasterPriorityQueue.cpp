#include "nwnx.hpp"

#include "API/CServerAIMaster.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CWorldTimer.hpp"
#include "API/CExoLinkedListInternal.hpp"

#include <queue>
#include <vector>

namespace Optimizations
{

using namespace NWNXLib;
using namespace NWNXLib::API;

// The engine keeps every pending AI event (DelayCommand, AssignCommand, effect expiry, combat rounds...) in
// CServerAIMaster::m_lEventQueue, a linked list sorted by world time. Inserting walks the list, so it costs
// O(queue length). This replaces the list with a binary heap.
//
// Events due at the same world time must run in exactly the order stock gives them (two DelayCommand(1.0)
// in one script get the same time). In the 8193.37 server:
//
//   AddEventAbsoluteTime (AddEventDeltaTime uses it for a zero delay) walks from the head and inserts
//   before the first event that is strictly later: after every event due at the same time.
//
//   AddEventAbsoluteTimeViaTail (every non-zero delay) walks from the tail and inserts after the first event
//   that is strictly earlier: BEFORE every event due at the same time.
//
// So within one world time the list holds the ViaTail events newest first, then the head-walk events oldest
// first. Each node gets a sequence number that reproduces that: head-walk inserts count up from 1, ViaTail
// inserts count down from -1, and the heap orders by (day, time, sequence).

struct EventNode
{
    uint32_t m_nCalendarDay;
    uint32_t m_nTimeOfDay;
    int64_t  m_nSequence;
    OBJECT_ID m_nCallerObjectId;
    OBJECT_ID m_nObjectId;
    uint32_t m_nEventId;
    void *m_pEventData;
};

struct EventNodeLater
{
    bool operator()(const EventNode& lhs, const EventNode& rhs) const
    {
        if (lhs.m_nCalendarDay != rhs.m_nCalendarDay) return lhs.m_nCalendarDay > rhs.m_nCalendarDay;
        if (lhs.m_nTimeOfDay != rhs.m_nTimeOfDay) return lhs.m_nTimeOfDay > rhs.m_nTimeOfDay;
        return lhs.m_nSequence > rhs.m_nSequence;
    }
};

using EventQueue = std::priority_queue<EventNode, std::vector<EventNode>, EventNodeLater>;
static EventQueue s_EventQueue;
static int64_t s_HeadSequence = 0;
static int64_t s_TailSequence = 0;

static Hooks::Hook s_SaveEventQueueHook;
static Hooks::Hook s_LoadEventQueueHook;


// The list itself stays empty (its head and tail stay null), but its count follows the heap, so readers of the
// count (NWNX_Profiler's AIQueuedEvents) still see the real queue length. The engine only walks the list by its
// head pointer, never by the count.
static void SyncCount(CServerAIMaster *pThis)
{
    pThis->m_lEventQueue.m_pcExoLinkedListInternal->m_nCount = static_cast<uint32_t>(s_EventQueue.size());
}

static void Push(uint32_t nCalendarDay, uint32_t nTimeOfDay, int64_t nSequence, OBJECT_ID nCallerObjectId, OBJECT_ID nObjectId, uint32_t nEventId, void *pEventData)
{
    s_EventQueue.push(EventNode { nCalendarDay, nTimeOfDay, nSequence, nCallerObjectId, nObjectId, nEventId, pEventData });
}

static void ClearEventQueue(CServerAIMaster *pThis)
{
    // As stock: free each event's data.
    while (!s_EventQueue.empty())
    {
        EventNode node = s_EventQueue.top();
        s_EventQueue.pop();
        pThis->DeleteEventData(node.m_nEventId, node.m_pEventData);
    }
    s_EventQueue = {};
    s_HeadSequence = 0;
    s_TailSequence = 0;
    SyncCount(pThis);
}

static BOOL AddEventAbsoluteTime(CServerAIMaster *pThis, uint32_t nCalendarDay, uint32_t nTimeOfDay, OBJECT_ID nCallerObjectId, OBJECT_ID nObjectId, uint32_t nEventId, void *pEventData)
{
    Push(nCalendarDay, nTimeOfDay, ++s_HeadSequence, nCallerObjectId, nObjectId, nEventId, pEventData);
    SyncCount(pThis);
    return true;
}

static BOOL AddEventAbsoluteTimeViaTail(CServerAIMaster *pThis, uint32_t nCalendarDay, uint32_t nTimeOfDay, OBJECT_ID nCallerObjectId, OBJECT_ID nObjectId, uint32_t nEventId, void *pEventData)
{
    Push(nCalendarDay, nTimeOfDay, --s_TailSequence, nCallerObjectId, nObjectId, nEventId, pEventData);
    SyncCount(pThis);
    return true;
}

static BOOL EventPending(CServerAIMaster *pThis, uint32_t nCalendarDay, uint32_t nTimeOfDay)
{
    if (s_EventQueue.empty())
        return false;

    const EventNode& node = s_EventQueue.top();
    return pThis->m_pExoAppInternal->m_pWorldTimer->CompareWorldTimes(node.m_nCalendarDay, node.m_nTimeOfDay, nCalendarDay, nTimeOfDay) <= 0;
}

static BOOL GetPendingEvent(CServerAIMaster *pThis, uint32_t *nCalendarDay, uint32_t *nTimeOfDay, OBJECT_ID *nCallerObjectId, OBJECT_ID *nObjectId, uint32_t *nEventId, void **pEventData)
{
    if (s_EventQueue.empty())
        return false;

    const EventNode& node = s_EventQueue.top();
    *nCalendarDay    = node.m_nCalendarDay;
    *nTimeOfDay      = node.m_nTimeOfDay;
    *nCallerObjectId = node.m_nCallerObjectId;
    *nObjectId       = node.m_nObjectId;
    *nEventId        = node.m_nEventId;
    *pEventData      = node.m_pEventData;
    s_EventQueue.pop();
    SyncCount(pThis);
    return true;
}

// The engine's only other users of m_lEventQueue are SaveEventQueue (CNWSModule::SaveModuleIFOStart: saving a
// game) and LoadEventQueue (CNWSModule::LoadModuleStart: an EventQueue list in module.ifo, from a saved game). Both
// touch the list directly, so the list is filled for the save, and emptied into the heap after a load.
static int32_t SaveEventQueue(CServerAIMaster *pThis, CResGFF *pRes, CResStruct *pStruct)
{
    auto *pList = pThis->m_lEventQueue.m_pcExoLinkedListInternal;
    pList->m_nCount = 0;
    EventQueue copy = s_EventQueue;
    while (!copy.empty())
    {
        const EventNode& node = copy.top();
        pList->AddTail(new CServerAIEventNode { node.m_nCalendarDay, node.m_nTimeOfDay, node.m_nCallerObjectId, node.m_nObjectId, node.m_nEventId, node.m_pEventData });
        copy.pop();
    }

    auto ret = s_SaveEventQueueHook->CallOriginal<int32_t>(pThis, pRes, pStruct);

    while (pList->pHead)
        delete static_cast<CServerAIEventNode*>(pList->RemoveHead());
    SyncCount(pThis);
    return ret;
}

static int32_t LoadEventQueue(CServerAIMaster *pThis, CResGFF *pRes, CResStruct *pStruct)
{
    auto *pList = pThis->m_lEventQueue.m_pcExoLinkedListInternal;
    pList->m_nCount = 0;
    auto ret = s_LoadEventQueueHook->CallOriginal<int32_t>(pThis, pRes, pStruct);

    // The list was saved in order; keep it (the save's order is the order the events ran in).
    while (pList->pHead)
    {
        auto *pNode = static_cast<CServerAIEventNode*>(pList->RemoveHead());
        Push(pNode->m_nCalendarDay, pNode->m_nTimeOfDay, ++s_HeadSequence, pNode->m_nCallerObjectId, pNode->m_nObjectId, pNode->m_nEventId, pNode->m_pEventData);
        delete pNode;
    }
    SyncCount(pThis);
    return ret;
}

void EventMasterPriorityQueue() __attribute__((constructor));
void EventMasterPriorityQueue()
{
    if (!Config::Get<bool>("EVENT_MASTER_PRIORITY_QUEUE", false))
        return;

    LOG_INFO("Using a priority queue for the AI event queue");

    static Hooks::Hook s_ClearEventQueueHook = Hooks::HookFunction(&CServerAIMaster::ClearEventQueue, &ClearEventQueue, Hooks::Order::Final);
    static Hooks::Hook s_AddEventAbsoluteTimeHook = Hooks::HookFunction(&CServerAIMaster::AddEventAbsoluteTime, &AddEventAbsoluteTime, Hooks::Order::Final);
    static Hooks::Hook s_AddEventAbsoluteTimeViaTailHook = Hooks::HookFunction(&CServerAIMaster::AddEventAbsoluteTimeViaTail, &AddEventAbsoluteTimeViaTail, Hooks::Order::Final);
    static Hooks::Hook s_EventPendingHook = Hooks::HookFunction(&CServerAIMaster::EventPending, &EventPending, Hooks::Order::Final);
    static Hooks::Hook s_GetPendingEventHook = Hooks::HookFunction(&CServerAIMaster::GetPendingEvent, &GetPendingEvent, Hooks::Order::Final);
    s_SaveEventQueueHook = Hooks::HookFunction(&CServerAIMaster::SaveEventQueue, &SaveEventQueue, Hooks::Order::Early);
    s_LoadEventQueueHook = Hooks::HookFunction(&CServerAIMaster::LoadEventQueue, &LoadEventQueue, Hooks::Order::Early);
}

}
