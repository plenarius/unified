#pragma once

#include "nwnx.hpp"

#include <chrono>

namespace Profiler {

class PathingOutcomes
{
public:
    PathingOutcomes(NWNXLib::Services::MetricsProxy* metrics, std::chrono::seconds interval);
};

}
