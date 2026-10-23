#include "nwnx.hpp"

#include "API/CNWRules.hpp"
#include "API/CNWSItem.hpp"
#include "API/CNWBaseItem.hpp"
#include "API/CNWBaseItemArray.hpp"
#include "API/CExoResMan.hpp"
#include "API/CExoString.hpp"
#include "API/CExoStringList.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace NWNXLib;
using namespace NWNXLib::API;

namespace {

// baseitems.2da ModelType
enum ModelType : uint8_t
{
    Simple    = 0,
    Layered   = 1,
    Composite = 2,
    Armor     = 3,
};

constexpr int32_t NUM_PARTS = 3;
constexpr int32_t RANDOM_WEAPON_TRIES = 1000;

// Inclusive value ranges. Empty means any value.
using Ranges = std::vector<std::pair<int32_t, int32_t>>;

struct Rule
{
    std::array<Ranges, NUM_PARTS> models; // simple and layered items use models[0] only
    std::array<Ranges, NUM_PARTS> colors;
};

struct Rules
{
    std::vector<Rule> blacklist;
    std::vector<Ranges> sync;
};

struct Appearances
{
    // Simple and layered items: model numbers. Composite items: model * 10 + color per part.
    // Both sorted and unique.
    std::vector<int32_t> simple;
    std::array<std::vector<int32_t>, NUM_PARTS> parts;
};

// Both keyed by the lowercase baseitems.2da ItemClass, like the icon and model names.
std::unordered_map<std::string, Appearances> s_appearances;
std::unordered_map<std::string, Rules> s_rules;
std::mt19937 s_rng{std::random_device{}()};

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// A non-negative number of up to six digits.
std::optional<int32_t> ParseNumber(const std::string& s)
{
    if (s.empty() || s.size() > 6 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return std::stoi(s);
}

CNWBaseItem* GetBaseItem(int32_t baseItem)
{
    auto *pBaseItemArray = Globals::Rules()->m_pBaseItemArray;
    if (baseItem < 0 || baseItem >= pBaseItemArray->m_nNumBaseItems)
        return nullptr;
    return pBaseItemArray->GetBaseItem(baseItem);
}

std::string GetItemClass(const CNWBaseItem *pBaseItem)
{
    return ToLower(pBaseItem->m_ItemClassResRefChunk);
}

template <typename F>
void ForEachResOfType(uint16_t type, F&& fn)
{
    if (auto *pList = Globals::ExoResMan()->GetResOfType(type, false))
    {
        for (int32_t i = 0; i < pList->m_nCount; i++)
            fn(ToLower(pList->m_pStrings[i]->CStr()));
        delete pList;
    }
}

// "*" is any value. Otherwise a comma separated list of "n", "n-m", "-m" (up to m) and "n-" (n and up).
std::optional<Ranges> ParseRanges(const std::string& str)
{
    Ranges ranges;
    if (str == "*")
        return ranges;

    size_t start = 0;
    while (start <= str.size())
    {
        auto end = str.find(',', start);
        if (end == std::string::npos)
            end = str.size();

        std::string token;
        for (auto c : str.substr(start, end - start))
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
                token += c;
        }

        const auto dash = token.find('-');
        const auto lower = token.substr(0, dash);
        const auto upper = dash == std::string::npos ? lower : token.substr(dash + 1);
        if (lower.empty() && upper.empty())
            return std::nullopt;

        const auto low = lower.empty() ? std::optional<int32_t>(0) : ParseNumber(lower);
        const auto high = upper.empty() ? std::optional<int32_t>(INT_MAX) : ParseNumber(upper);
        if (!low || !high || *low > *high)
            return std::nullopt;
        ranges.emplace_back(*low, *high);

        start = end + 1;
    }
    return ranges;
}

bool InRanges(const Ranges& ranges, int32_t value)
{
    if (ranges.empty())
        return true;
    return std::any_of(ranges.begin(), ranges.end(), [value](const auto& r) { return value >= r.first && value <= r.second; });
}

const Rules* GetRules(const std::string& itemClass)
{
    auto it = s_rules.find(itemClass);
    return it == s_rules.end() ? nullptr : &it->second;
}

bool IsSimpleAllowed(const Rules *pRules, int32_t value)
{
    if (!pRules)
        return true;
    return std::none_of(pRules->blacklist.begin(), pRules->blacklist.end(),
                        [value](const Rule& rule) { return InRanges(rule.models[0], value); });
}

bool IsSynced(const Rules *pRules, int32_t model)
{
    return pRules && std::any_of(pRules->sync.begin(), pRules->sync.end(),
                                 [model](const Ranges& sync) { return InRanges(sync, model); });
}

// values: model * 10 + color for bottom, middle and top.
bool IsWeaponAllowed(const Rules *pRules, const std::array<int32_t, NUM_PARTS>& values)
{
    if (!pRules)
        return true;

    for (const auto& rule : pRules->blacklist)
    {
        bool match = true;
        for (int32_t k = 0; k < NUM_PARTS && match; k++)
            match = InRanges(rule.models[k], values[k] / 10) && InRanges(rule.colors[k], values[k] % 10);
        if (match)
            return false;
    }

    // A synced model must be used by all three parts.
    const bool allSame = values[0] / 10 == values[1] / 10 && values[1] / 10 == values[2] / 10;
    for (int32_t k = 0; k < NUM_PARTS; k++)
    {
        if (!allSame && IsSynced(pRules, values[k] / 10))
            return false;
    }
    return true;
}

struct ItemInfo
{
    CNWSItem *pItem;
    uint8_t modelType;
    const Appearances *pAppearances;
    const Rules *pRules;
};

// part is only used by composite items.
std::optional<ItemInfo> GetItemInfo(CNWSItem *pItem, int32_t part)
{
    if (!pItem)
        return std::nullopt;
    auto *pBaseItem = GetBaseItem(pItem->m_nBaseItem);
    if (!pBaseItem)
        return std::nullopt;

    const auto itemClass = GetItemClass(pBaseItem);
    auto it = s_appearances.find(itemClass);
    if (it == s_appearances.end())
        return std::nullopt;
    if (pBaseItem->m_nModelType == ModelType::Composite && (part < 0 || part >= NUM_PARTS))
    {
        LOG_WARNING("Invalid weapon part %d", part);
        return std::nullopt;
    }
    return ItemInfo{pItem, pBaseItem->m_nModelType, &it->second, GetRules(itemClass)};
}

// The values the item's part can take without breaking a rule, keeping its other parts. Sorted.
std::vector<int32_t> GetAllowed(const ItemInfo& info, int32_t part)
{
    std::vector<int32_t> allowed;
    switch (info.modelType)
    {
        case ModelType::Simple:
        case ModelType::Layered:
            for (auto value : info.pAppearances->simple)
            {
                if (IsSimpleAllowed(info.pRules, value))
                    allowed.push_back(value);
            }
            break;

        case ModelType::Composite:
            for (auto value : info.pAppearances->parts[part])
            {
                std::array<int32_t, NUM_PARTS> values = {info.pItem->m_nModelPart[0], info.pItem->m_nModelPart[1], info.pItem->m_nModelPart[2]};
                values[part] = value;
                if (IsWeaponAllowed(info.pRules, values))
                    allowed.push_back(value);
            }
            break;

        default:
            break;
    }
    return allowed;
}

int32_t GetCurrentValue(const ItemInfo& info, int32_t part)
{
    return info.modelType == ModelType::Composite ? info.pItem->m_nModelPart[part] : info.pItem->m_nModelPart[0];
}

// The next (or previous) value after current in sorted values, wrapping around. -1 if values is empty.
int32_t Step(const std::vector<int32_t>& values, int32_t current, bool next)
{
    if (values.empty())
        return -1;
    if (next)
    {
        auto it = std::upper_bound(values.begin(), values.end(), current);
        return it == values.end() ? values.front() : *it;
    }
    auto it = std::lower_bound(values.begin(), values.end(), current);
    return it == values.begin() ? values.back() : *std::prev(it);
}

int32_t Pick(const std::vector<int32_t>& values)
{
    if (values.empty())
        return -1;
    std::uniform_int_distribution<size_t> dist(0, values.size() - 1);
    return values[dist(s_rng)];
}

int32_t NextPreviousAppearance(ArgumentStack& args, bool next)
{
    auto *pItem = Utils::PopItem(args);
    const auto part = args.extract<int32_t>();

    auto info = GetItemInfo(pItem, part);
    if (!info)
        return -1;

    const auto allowed = GetAllowed(*info, part);
    if (allowed.empty())
        return -1;

    const auto current = GetCurrentValue(*info, part);
    if (info->modelType != ModelType::Composite)
        return Step(allowed, current, next);

    std::vector<int32_t> models;
    for (auto value : allowed)
    {
        if (models.empty() || models.back() != value / 10)
            models.push_back(value / 10);
    }
    const auto model = Step(models, current / 10, next);

    // Keep the current color if the new model has it, otherwise its first allowed color.
    if (std::binary_search(allowed.begin(), allowed.end(), model * 10 + current % 10))
        return model * 10 + current % 10;
    return *std::lower_bound(allowed.begin(), allowed.end(), model * 10);
}

int32_t NextPreviousColor(ArgumentStack& args, bool next)
{
    auto *pItem = Utils::PopItem(args);
    const auto part = args.extract<int32_t>();

    auto info = GetItemInfo(pItem, part);
    if (!info)
        return -1;

    const auto allowed = GetAllowed(*info, part);
    const auto current = GetCurrentValue(*info, part);
    if (info->modelType != ModelType::Composite)
        return Step(allowed, current, next);

    std::vector<int32_t> colors;
    for (auto value : allowed)
    {
        if (value / 10 == current / 10)
            colors.push_back(value % 10);
    }
    return Step(colors, current % 10, next);
}

}

NWNX_EXPORT ArgumentStack CacheAppearances(ArgumentStack&&)
{
    s_appearances.clear();
    s_rules.clear();

    // Icons are i<class>_<number> for simple and layered items and i<class>_<b|m|t>_<number> for
    // composite ones; composite models are <class>_<b|m|t>_<number>.
    std::unordered_map<std::string, std::set<int32_t>> simpleIcons;
    std::unordered_map<std::string, std::array<std::set<std::string>, NUM_PARTS>> partIcons;
    auto addIcon = [&](const std::string& name)
    {
        if (name.size() < 3 || name[0] != 'i')
            return;
        const auto underscore = name.find_last_of('_');
        const auto number = underscore == std::string::npos ? std::nullopt : ParseNumber(name.substr(underscore + 1));
        if (!number)
            return;

        // Simple model 0 can't be set.
        const auto prefix = name.substr(1, underscore - 1);
        if (*number > 0)
            simpleIcons[prefix].insert(*number);

        static const std::string partLetters = "bmt";
        if (prefix.size() > 2 && prefix[prefix.size() - 2] == '_')
        {
            const auto part = partLetters.find(prefix.back());
            if (part != std::string::npos)
                partIcons[prefix.substr(0, prefix.size() - 2)][part].insert(name.substr(1));
        }
    };
    ForEachResOfType(Constants::ResRefType::TGA, addIcon);
    ForEachResOfType(Constants::ResRefType::DDS, addIcon);
    ForEachResOfType(Constants::ResRefType::PLT, addIcon);

    std::unordered_set<std::string> models;
    ForEachResOfType(Constants::ResRefType::MDL, [&](const std::string& name) { models.insert(name); });

    auto *pBaseItemArray = Globals::Rules()->m_pBaseItemArray;
    for (int32_t baseItem = 0; baseItem < pBaseItemArray->m_nNumBaseItems; baseItem++)
    {
        auto *pBaseItem = pBaseItemArray->GetBaseItem(baseItem);
        if (!pBaseItem || !pBaseItem->m_nName)
            continue;

        const auto itemClass = GetItemClass(pBaseItem);
        if (itemClass.empty() || s_appearances.count(itemClass))
            continue;

        auto& appearances = s_appearances[itemClass];
        switch (pBaseItem->m_nModelType)
        {
            case ModelType::Simple:
            case ModelType::Layered:
            {
                std::set<int32_t> numbers;
                auto it = simpleIcons.find(itemClass);
                if (it != simpleIcons.end())
                    numbers = it->second;
                // Cloak icons are icloak_m_<number>.
                it = simpleIcons.find(itemClass + "_m");
                if (pBaseItem->m_nModelType == ModelType::Layered && it != simpleIcons.end())
                    numbers.insert(it->second.begin(), it->second.end());
                appearances.simple.assign(numbers.begin(), numbers.end());
                break;
            }
            case ModelType::Composite:
            {
                auto it = partIcons.find(itemClass);
                if (it == partIcons.end())
                    break;

                // Items whose parts are drawn (weapons) need a model for each icon; items that are
                // never drawn (boots) have none.
                bool hasModels = false;
                for (const auto& part : it->second)
                {
                    for (const auto& name : part)
                        hasModels = hasModels || models.count(name);
                }

                for (int32_t k = 0; k < NUM_PARTS; k++)
                {
                    for (const auto& name : it->second[k])
                    {
                        if (hasModels && !models.count(name))
                        {
                            LOG_WARNING("You have an icon for i%s but no model.", name.c_str());
                            continue;
                        }
                        appearances.parts[k].push_back(*ParseNumber(name.substr(name.find_last_of('_') + 1)));
                    }
                    std::sort(appearances.parts[k].begin(), appearances.parts[k].end());
                    appearances.parts[k].erase(std::unique(appearances.parts[k].begin(), appearances.parts[k].end()), appearances.parts[k].end());
                }
                break;
            }
            default:
                break;
        }
    }

    LOG_INFO("Cached appearances for %zu item classes.", s_appearances.size());
    return {};
}

NWNX_EXPORT ArgumentStack BlacklistAppearance(ArgumentStack&& args)
{
    const auto baseItem = args.extract<int32_t>();
    std::array<std::string, NUM_PARTS * 2> ranges;
    for (auto& range : ranges)
        range = args.extract<std::string>();

    auto *pBaseItem = GetBaseItem(baseItem);
    if (!pBaseItem)
    {
        LOG_WARNING("BlacklistAppearance: invalid base item %d", baseItem);
        return {};
    }

    Rule rule;
    for (int32_t k = 0; k < NUM_PARTS * 2; k++)
    {
        auto parsed = ParseRanges(ranges[k]);
        if (!parsed)
        {
            LOG_WARNING("BlacklistAppearance: base item %d: invalid range '%s', rule ignored", baseItem, ranges[k].c_str());
            return {};
        }
        (k < NUM_PARTS ? rule.models[k] : rule.colors[k - NUM_PARTS]) = *parsed;
    }
    s_rules[GetItemClass(pBaseItem)].blacklist.push_back(std::move(rule));
    return {};
}

NWNX_EXPORT ArgumentStack SyncAppearance(ArgumentStack&& args)
{
    const auto baseItem = args.extract<int32_t>();
    const auto models = args.extract<std::string>();

    auto *pBaseItem = GetBaseItem(baseItem);
    if (!pBaseItem)
    {
        LOG_WARNING("SyncAppearance: invalid base item %d", baseItem);
        return {};
    }

    auto parsed = ParseRanges(models);
    if (!parsed)
    {
        LOG_WARNING("SyncAppearance: base item %d: invalid range '%s', ignored", baseItem, models.c_str());
        return {};
    }
    s_rules[GetItemClass(pBaseItem)].sync.push_back(std::move(*parsed));
    return {};
}

NWNX_EXPORT ArgumentStack GetRandomAppearance(ArgumentStack&& args)
{
    auto *pItem = Utils::PopItem(args);
    const auto part = args.extract<int32_t>();

    auto info = GetItemInfo(pItem, part);
    if (!info)
        return -1;
    return Pick(GetAllowed(*info, part));
}

NWNX_EXPORT ArgumentStack GetRandomWeaponAppearance(ArgumentStack&& args)
{
    std::array<int32_t, NUM_PARTS> values = {-1, -1, -1};

    auto info = GetItemInfo(Utils::PopItem(args), 0);
    if (!info || info->modelType != ModelType::Composite)
        return {values[0], values[1], values[2]};

    const auto& parts = info->pAppearances->parts;
    if (parts[0].empty() || parts[1].empty() || parts[2].empty())
        return {values[0], values[1], values[2]};

    // Pick the bottom first. A synced model there is used for all three parts; otherwise synced
    // models are left out of the middle and top.
    for (int32_t i = 0; i < RANDOM_WEAPON_TRIES; i++)
    {
        std::array<int32_t, NUM_PARTS> candidate;
        candidate[0] = Pick(parts[0]);
        const auto model = candidate[0] / 10;
        const bool synced = IsSynced(info->pRules, model);

        bool ok = true;
        for (int32_t k = 1; k < NUM_PARTS && ok; k++)
        {
            std::vector<int32_t> choices;
            for (auto value : parts[k])
            {
                if (synced ? value / 10 == model : !IsSynced(info->pRules, value / 10))
                    choices.push_back(value);
            }
            candidate[k] = Pick(choices);
            ok = candidate[k] != -1;
        }

        if (ok && IsWeaponAllowed(info->pRules, candidate))
        {
            values = candidate;
            break;
        }
    }

    if (values[0] == -1)
        LOG_WARNING("GetRandomWeaponAppearance: no allowed appearance found for base item %d", info->pItem->m_nBaseItem);
    return {values[0], values[1], values[2]};
}

NWNX_EXPORT ArgumentStack GetNextAppearance(ArgumentStack&& args)
{
    return NextPreviousAppearance(args, true);
}

NWNX_EXPORT ArgumentStack GetPreviousAppearance(ArgumentStack&& args)
{
    return NextPreviousAppearance(args, false);
}

NWNX_EXPORT ArgumentStack GetNextColor(ArgumentStack&& args)
{
    return NextPreviousColor(args, true);
}

NWNX_EXPORT ArgumentStack GetPreviousColor(ArgumentStack&& args)
{
    return NextPreviousColor(args, false);
}
