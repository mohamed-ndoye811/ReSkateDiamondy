// Checks the cleanup (Launcher/mod_cleanup.h): which mods a plan switches off and which it keeps and
// why, that a mod another needs is never switched off, that mods.json keeps its order, and that a
// snapshot is taken (and old ones dropped) before anything changes, with the profile in use left alone.
#include "Launcher/mod_cleanup.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

namespace cleanup = dingosdk::launcher_cleanup;
namespace profiles = dingosdk::launcher_profiles;
using namespace dingosdk::mods;

constexpr std::int64_t day = 24 * 60 * 60;
constexpr std::int64_t now = 1'790'000'000 + 100 * day;

ModList list_of(const std::vector<std::pair<std::string, bool>>& rows) {
    ModList list;
    list.present = true;
    for (const auto& [name, enabled] : rows) {
        ModEntry entry;
        entry.mod.name = name;
        entry.mod.version = "1.0.0";
        entry.enabled = enabled;
        list.entries.push_back(entry);
    }
    return list;
}

ModCost video(std::uint64_t megabytes) {
    ModCost cost;
    cost.texture_bytes = megabytes << 20;
    return cost;
}

using Names = std::vector<std::string>;
} // namespace

int main() try {
    const auto list = list_of({{"Big-Map", true}, {"Old-Hat", true}, {"Core-Library", true}, {"Uses-Library", true},
                               {"Off-Already", false}, {"New-Deck", true}, {"Plugin", true}});

    // Unused for 30 days.
    {
        Usage usage;
        usage["Big-Map"] = {now - 200 * day, {{UseKind::map, {now - 2 * day, 5}}}};
        usage["Old-Hat"] = {now - 200 * day, {{UseKind::item, {now - 90 * day, 3}}}};
        usage["Core-Library"].first_seen = now - 200 * day;     // never of use itself
        usage["Uses-Library"].first_seen = now - 200 * day;     // nor this one
        usage["Off-Already"].first_seen = now - 200 * day;
        usage["New-Deck"].first_seen = now - 3 * day;            // just installed
        // "Plugin": never seen

        auto plan = cleanup::unused_plan(list, usage, 30, now, {});
        check(plan.switch_off == Names{"Old-Hat", "Core-Library", "Uses-Library"},
            "unused for 30 days: enabled ones only, in load order");
        check(plan.kept.empty(), "nothing needs them");

        // A mod that stays needs the library: it stays, and says why.
        plan = cleanup::unused_plan(list, usage, 30, now, {{"big-map", {"CORE-library"}}});
        check(plan.switch_off == Names{"Old-Hat", "Uses-Library"}, "a mod a staying one needs is kept, ignoring case");
        check(plan.kept.size() == 1 && plan.kept.at("Core-Library") == "needed by Big-Map", "and the plan says why");

        // A mod that goes does not keep what it needs.
        plan = cleanup::unused_plan(list, usage, 30, now, {{"Uses-Library", {"Core-Library"}}});
        check(plan.switch_off == Names{"Old-Hat", "Core-Library", "Uses-Library"}, "both go when the one needing it goes too");

        // Needs in a chain: what a kept mod needs is kept too.
        plan = cleanup::unused_plan(list, usage, 30, now, {{"Big-Map", {"Uses-Library"}}, {"Uses-Library", {"Core-Library"}}});
        check(plan.switch_off == Names{"Old-Hat"}, "a kept mod keeps what it needs");
        check(plan.kept.at("Core-Library") == "needed by Uses-Library", "the chain is named link by link");

        check(cleanup::unused_plan(list, usage, 365, now, {}).empty(), "nothing is unused for a year");
    }

    // Light: the heaviest go until the rest fit.
    {
        Costs costs;
        costs["Big-Map"] = video(3000);
        costs["Old-Hat"] = video(400);
        costs["Core-Library"] = video(0);
        costs["Uses-Library"] = video(400);
        costs["Off-Already"] = video(9000);   // already off: not counted
        costs["Plugin"] = video(0);
        // "New-Deck" not measured

        auto plan = cleanup::light_plan(list, costs, 1000ull << 20, {});
        check(plan.switch_off == Names{"Big-Map"}, "3800 MB down to 800 MB: only the map goes");
        check(plan.kept.size() == 1 && plan.kept.at("New-Deck") == "not measured yet", "a mod not measured stays");

        plan = cleanup::light_plan(list, costs, 500ull << 20, {});
        check(plan.switch_off == Names{"Big-Map", "Uses-Library"},
            "of two as heavy, the one lower in the load order goes first; the list is in load order");

        plan = cleanup::light_plan(list, costs, 0, {});
        check(plan.switch_off == Names{"Big-Map", "Old-Hat", "Uses-Library"}, "a budget of 0: every mod with video memory to give back");

        plan = cleanup::light_plan(list, costs, 8000ull << 20, {});
        check(plan.empty(), "everything fits: nothing goes");

        plan = cleanup::light_plan(list, costs, 1000ull << 20, {{"Plugin", {"Big-Map"}}});
        check(plan.empty() && plan.kept.at("Big-Map") == "needed by Plugin", "a heavy mod another needs stays");
    }

    // mods.json once a plan is made.
    {
        cleanup::Plan plan;
        plan.switch_off = {"old-hat", "Plugin"};
        const auto entries = cleanup::switched_off(list, plan);
        check(entries.size() == list.entries.size(), "every mod is still listed");
        for (std::size_t at = 0; at < entries.size(); ++at)
            check(entries[at].mod.name == list.entries[at].mod.name, "the load order is as it was");
        check(!entries[1].enabled && !entries[6].enabled, "the plan's mods are off, ignoring case");
        check(entries[0].enabled && !entries[4].enabled && entries[5].enabled, "the others are as they were");
    }

    // Snapshots.
    {
        profiles::Profiles saved;
        saved.list.push_back({"Street maps", {{"Big-Map", "1.0.0", true}}});
        saved.active = "Street maps";
        const auto before = saved;

        std::string name;
        check(cleanup::add_snapshot(saved, list, "2026-10-25 18h40", name), "a snapshot is taken");
        check(name == "Before cleanup 2026-10-25 18h40" && cleanup::is_snapshot(name), "named for when it was taken");
        check(profiles::valid_profile_name(name), "a name a profile may have");
        check(saved.active == "Street maps" && saved.list.front() == before.list.front(), "the profile in use is left alone");
        const auto& snapshot = saved.list.back();
        check(snapshot.mods.size() == list.entries.size() && snapshot.mods[4].enabled == false && snapshot.mods[0].enabled,
            "the snapshot holds every mod as it is, switches too");

        check(cleanup::add_snapshot(saved, list, "2026-10-25 18h40", name) && name == "Before cleanup 2026-10-25 18h40 (2)",
            "two in the same minute get different names");
        check(cleanup::add_snapshot(saved, list, "2026-10-26 09h00", name), "a third");
        check(cleanup::add_snapshot(saved, list, "2026-10-27 09h00", name), "a fourth");
        std::size_t snapshots{};
        for (const auto& profile : saved.list) snapshots += cleanup::is_snapshot(profile.name) ? 1u : 0u;
        check(snapshots == cleanup::kept_snapshots, "only the last few snapshots are kept");
        check(saved.list[1].name == "Before cleanup 2026-10-25 18h40 (2)", "the oldest went first");
        check(saved.list.front().name == "Street maps", "the player's own profile is never dropped");

        // A snapshot in use is never dropped.
        saved.active = saved.list[1].name;
        check(cleanup::add_snapshot(saved, list, "2026-10-28 09h00", name), "a fifth");
        check(std::ranges::any_of(saved.list, [&](const auto& p) { return p.name == saved.active; }), "the snapshot in use stays");

        // No room: refused, nothing changed.
        profiles::Profiles full;
        for (std::size_t at = 0; at < profiles::max_profiles; ++at) full.list.push_back({"P" + std::to_string(at), {}});
        const auto unchanged = full;
        check(!cleanup::add_snapshot(full, list, "2026-10-25 18h40", name), "no room for a snapshot: refused");
        check(full.list == unchanged.list && full.active == unchanged.active, "and nothing changed");
    }

    // A light profile beside the others.
    {
        profiles::Profiles saved;
        saved.list.push_back({"Street maps (light)", {}});
        cleanup::Plan plan;
        plan.switch_off = {"Big-Map"};
        const auto light = cleanup::light_profile(saved, list, plan, "Street maps");
        check(light.name == "Street maps (light) (2)", "a light profile never takes a name in use");
        check(!light.mods[0].enabled && light.mods[1].enabled && !light.mods[4].enabled, "the plan made, the rest as it is");
        check(saved.list.size() == 1, "it is not added by itself");

        const auto long_name = cleanup::light_profile({}, list, plan, std::string(60, 'x'));
        check(long_name.name.size() <= profiles::max_name_bytes && long_name.name.ends_with(" (light)"),
            "a long name is cut, keeping (light): " + long_name.name);
        check(profiles::valid_profile_name(long_name.name), "still a name a profile may have");
        check(cleanup::light_profile({}, list, plan, "a:b").name == "ab (light)", "characters a file name refuses are dropped");
    }

    if (failures) {
        std::cerr << failures << " mod cleanup check(s) failed\n";
        return 1;
    }
    std::cout << "mod cleanup checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
