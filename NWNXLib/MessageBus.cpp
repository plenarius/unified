#include "nwnx.hpp"
#include <algorithm>
#include <mutex>

namespace NWNXLib::MessageBus
{

using HandlerList = std::vector<std::pair<uint32_t, Handler>>;
using MessageMap = std::unordered_map<std::string, HandlerList>;
static MessageMap s_messageMap;
static std::mutex s_subscribedMutex;

static std::atomic<bool>& SubscribedFlag(const std::string& tag)
{
    // Never freed, so the references handed out stay valid during static destruction.
    static auto* s_subscribed = new std::unordered_map<std::string, std::atomic<bool>>();
    std::lock_guard<std::mutex> lock(s_subscribedMutex);
    return s_subscribed->try_emplace(tag, false).first->second;
}

uint32_t Subscribe(const std::string& tag, const Handler& handler)
{
    static uint32_t s_nextHandlerId = 0;
    uint32_t nextId = s_nextHandlerId++;
    s_messageMap[tag].push_back(std::make_pair(nextId, handler));
    SubscribedFlag(tag).store(true, std::memory_order_relaxed);
    return nextId;
}

void Unsubscribe(const uint32_t id)
{
    for (auto& bucket : s_messageMap)
    {
        HandlerList& handlers = bucket.second;
        auto entry = std::find_if(std::begin(handlers), std::end(handlers), [id](const auto& elem)
        {
            return elem.first == id;
        });

        if (entry != std::end(handlers))
        {
            handlers.erase(entry);
            SubscribedFlag(bucket.first).store(!handlers.empty(), std::memory_order_relaxed);
            return;
        }
    }

    throw std::runtime_error("Tried to unsubscribe with an ID that wasn't present.");
}

void Broadcast(const std::string& tag, const Message& message)
{
    auto bucket = s_messageMap.find(tag);

    if (bucket == std::end(s_messageMap))
    {
        return;
    }

    for (std::pair<uint32_t, Handler>& entry : bucket->second)
    {
        entry.second(message);
    }
}

bool HasSubscribers(const std::string& tag)
{
    return SubscribedFlag(tag).load(std::memory_order_relaxed);
}

const std::atomic<bool>& GetSubscribedFlag(const std::string& tag)
{
    return SubscribedFlag(tag);
}

}
