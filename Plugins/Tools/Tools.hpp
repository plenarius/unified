#pragma once

#include "Plugin.hpp"
#include "Services/Events/Events.hpp"
#include "Services/Hooks/Hooks.hpp"

using ArgumentStack = NWNXLib::Services::Events::ArgumentStack;

namespace Tools {

class Tools : public NWNXLib::Plugin
{
public:
    Tools(const Plugin::CreateParams& params);
    virtual ~Tools();

private:
    ArgumentStack GenerateMiniMaps (ArgumentStack&& args);
    ArgumentStack GenerateAreaGraph (ArgumentStack&& args);
};

}
