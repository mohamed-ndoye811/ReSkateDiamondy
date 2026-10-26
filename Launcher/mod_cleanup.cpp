#include "Launcher/mod_cleanup.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace dingosdk::launcher_cleanup {
namespace {
std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> enabled_mods(const mods::ModList& list) {
    std::vector<std::string> names;
    for (const auto& entry : list.entries)
        if (entry.enabled) names.push_back(entry.mod.name);
    return names;
}

// Takes back out of `plan` every mod that an enabled mod staying needs, and what that one needs in
// turn, saying which mod needs it.
void keep_needed(Plan& plan, const mods::ModList& list, const Dependencies& dependencies) {
    std::map<std::string, std::vector<std::string>> needs;   // folded names
    for (const auto& [mod, listed] : dependencies)
        for (const auto& dependency : listed) needs[folded(mod)].push_back(folded(dependency));
    std::map<std::string, std::string> going;                // folded -> as listed
    for (const auto& name : plan.switch_off) going.emplace(folded(name), name);
    std::vector<std::string> staying;                        // as listed
    for (const auto& name : enabled_mods(list))
        if (!going.contains(folded(name))) staying.push_back(name);
    for (std::size_t at = 0; at < staying.size(); ++at) {
        const auto found = needs.find(folded(staying[at]));
        if (found == needs.end()) continue;
        for (const auto& dependency : found->second) {
            const auto taken = going.find(dependency);
            if (taken == going.end()) continue;
            plan.kept.emplace(taken->second, "needed by " + staying[at]);
            staying.push_back(taken->second);
            going.erase(taken);
        }
    }
    std::erase_if(plan.switch_off, [&](const std::string& name) { return !going.contains(folded(name)); });
}

std::string with_suffix(std::string_view base, std::string_view suffix) {
    auto name = launcher_profiles::clean_profile_name(base);
    while (name.size() + suffix.size() > launcher_profiles::max_name_bytes) {
        // Never cut a UTF-8 character in half.
        do name.pop_back();
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80);
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return (name.empty() ? std::string("Profile") : name) + std::string(suffix);
}
} // namespace

bool watched_mod(const mods::Mod& mod, const mods::ModCost* cost) noexcept {
    if (mod.provides_music || mod.provides_objects) return false;
    return mod.provides_levels || mod.provides_plugin || (cost && cost->items);
}

Plan unused_plan(const mods::ModList& list, const mods::Usage& usage, int days, std::int64_t now,
                 const Dependencies& dependencies, const Names& watched, const Names& keep) {
    Plan plan;
    plan.switch_off = mods::unused_for(usage, enabled_mods(list), days, now);
    std::erase_if(plan.switch_off, [&](const std::string& name) {
        if (keep.contains(name)) plan.kept.emplace(name, "you keep it");
        else if (!watched.contains(name)) plan.kept.emplace(name, "its use cannot be seen");
        else return false;
        return true;
    });
    keep_needed(plan, list, dependencies);
    return plan;
}

Plan light_plan(const mods::ModList& list, const mods::Costs& costs, const Names& maps, std::uint64_t budget,
                const Dependencies& dependencies, const Names& keep) {
    Plan plan;
    const auto enabled = enabled_mods(list);
    mods::Costs staying;
    std::vector<std::pair<std::uint64_t, std::size_t>> heavy;   // video bytes, place in the load order
    for (std::size_t at = 0; at < enabled.size(); ++at) {
        const auto found = costs.find(enabled[at]);
        if (found == costs.end()) {
            plan.kept.emplace(enabled[at], "not measured yet");
            continue;
        }
        staying.emplace(enabled[at], found->second);
        const auto bytes = mods::video_bytes(found->second);
        if (bytes && keep.contains(enabled[at])) plan.kept.emplace(enabled[at], "you keep it");
        else if (bytes) heavy.emplace_back(bytes, at);
    }
    // Heaviest first; of two as heavy, the one lower in the load order goes first.
    std::ranges::sort(heavy, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    std::set<std::size_t> going;
    for (const auto& [bytes, at] : heavy) {
        const auto peak = mods::peak_video(staying, maps);
        if (peak.at_once() <= budget) break;
        // A map that is not the heaviest left gives nothing back: only one map is loaded at a time.
        if (maps.contains(enabled[at]) && peak.map != enabled[at]) continue;
        going.insert(at);
        staying.erase(enabled[at]);
    }
    for (const auto at : going) plan.switch_off.push_back(enabled[at]);   // std::set: load order
    keep_needed(plan, list, dependencies);
    return plan;
}

std::vector<mods::ModEntry> switched_off(const mods::ModList& list, const Plan& plan) {
    std::set<std::string> going;
    for (const auto& name : plan.switch_off) going.insert(folded(name));
    auto entries = list.entries;
    for (auto& entry : entries)
        if (going.contains(folded(entry.mod.name))) entry.enabled = false;
    return entries;
}

bool is_snapshot(std::string_view profile_name) noexcept { return profile_name.starts_with(snapshot_prefix); }

bool add_snapshot(launcher_profiles::Profiles& profiles, const mods::ModList& list, std::string_view when, std::string& name) {
    auto updated = profiles;
    auto& all = updated.list;
    // The oldest go first, leaving room for this one; a snapshot that is the profile in use stays.
    auto snapshots = static_cast<std::size_t>(std::ranges::count_if(all, [](const auto& profile) { return is_snapshot(profile.name); }));
    for (auto at = all.begin(); at != all.end() && snapshots >= kept_snapshots;) {
        if (is_snapshot(at->name) && at->name != updated.active) {
            at = all.erase(at);
            --snapshots;
        } else {
            ++at;
        }
    }
    if (all.size() >= launcher_profiles::max_profiles) return false;
    name = updated.free_name(with_suffix(std::string(snapshot_prefix) + std::string(when), ""));
    all.push_back(launcher_profiles::capture(name, list));
    profiles = std::move(updated);
    return true;
}

launcher_profiles::Profile light_profile(const launcher_profiles::Profiles& profiles, const mods::ModList& list,
                                         const Plan& plan, std::string_view base) {
    auto profile = launcher_profiles::capture(profiles.free_name(with_suffix(base, " (light)")), list);
    std::set<std::string> going;
    for (const auto& name : plan.switch_off) going.insert(folded(name));
    for (auto& mod : profile.mods)
        if (going.contains(folded(mod.name))) mod.enabled = false;
    return profile;
}

} // namespace dingosdk::launcher_cleanup
