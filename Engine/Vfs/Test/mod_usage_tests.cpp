// Checks how mod usage is noted and judged (Engine/Vfs/mod_usage.h): a use worn all afternoon counts
// once, a mod just installed or never seen is not "unused", the idle time runs from the last use of
// any kind, and Mods/.reskate-mod-usage.json reads back as written and survives a damaged entry.
#include "Engine/Vfs/mod_usage.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

namespace fs = std::filesystem;
using namespace dingosdk::mods;

constexpr std::int64_t day = 24 * 60 * 60;
constexpr std::int64_t start = 1'790'000'000;   // 2026-09

void write_text(const fs::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
}
} // namespace

int main() try {
    // Kinds have names both ways.
    for (const auto kind : use_kinds) check(use_kind_named(use_kind_name(kind)) == kind, "a kind reads back by its name");
    check(!use_kind_named("song"), "an unknown kind is none");

    // Noting uses.
    {
        UsageLog log;
        check(!log.changed(), "a new log has nothing to write");
        log.seen("Team-Map", start);
        check(log.changed() && log.usage().at("Team-Map").first_seen == start, "first seen is noted");
        log.mark_written();
        log.seen("Team-Map", start + day);
        check(!log.changed() && log.usage().at("Team-Map").first_seen == start, "seen again keeps the first time");

        log.used("Someone-Hat", UseKind::item, start + 100);
        const auto& hat = log.usage().at("Someone-Hat");
        check(hat.first_seen == start + 100, "a mod used before it was seen is seen then");
        check(hat.uses.at(UseKind::item) == Use{start + 100, 1}, "the first use is counted");

        log.used("Someone-Hat", UseKind::item, start + 400);
        check(hat.uses.at(UseKind::item) == Use{start + 400, 1}, "worn on a few minutes later: still one use, last time moves");
        log.used("Someone-Hat", UseKind::item, start + 400 + UsageLog::merge_seconds);
        check(hat.uses.at(UseKind::item).count == 2, "worn again after a pause: a second use");

        log.mark_written();
        log.used("Someone-Hat", UseKind::item, start - day);
        check(hat.uses.at(UseKind::item).last == start + 400 + UsageLog::merge_seconds, "a clock put back does not move the last use back");
        check(hat.uses.at(UseKind::item).count == 3, "but it is a use");

        log.used("Someone-Hat", UseKind::map, start + 2 * day);
        check(hat.uses.size() == 2, "each kind of use is kept apart");
        check(last_used(hat) == start + 2 * day, "the last use is the latest of any kind");
    }

    // Unused for N days.
    {
        Usage usage;
        usage["old-map"].first_seen = start;
        usage["old-map"].uses[UseKind::map] = {start + day, 4};
        usage["worn-hat"].first_seen = start;
        usage["worn-hat"].uses[UseKind::item] = {start + 40 * day, 9};
        usage["never-used"].first_seen = start;
        usage["just-installed"].first_seen = start + 39 * day;
        usage["blank"];   // known by name only

        const auto now = start + 41 * day;
        const std::vector<std::string> mods{"worn-hat", "untracked", "never-used", "old-map", "just-installed", "blank"};
        check(unused_for(usage, mods, 30, now) == std::vector<std::string>{"never-used", "old-map"},
            "unused for 30 days: last used, or never used and seen, that long ago; in the order asked");
        check(unused_for(usage, mods, 1, now) == std::vector<std::string>{"worn-hat", "never-used", "old-map", "just-installed"},
            "a day: everything with a date that old");
        check(unused_for(usage, mods, 0, now).size() == 4, "0 days: everything with a date");
        check(unused_for(usage, mods, -5, now) == unused_for(usage, mods, 0, now), "negative days are 0");
        check(unused_for(usage, mods, 365, now).empty(), "nothing is a year old");

        check(idle_days(usage, "old-map", now) == 40, "40 whole days since the map was loaded");
        check(idle_days(usage, "never-used", now) == 41, "a mod never used: days since it was first seen");
        check(!idle_days(usage, "untracked", now), "a mod never seen: unknown");
        check(!idle_days(usage, "blank", now), "a mod with no dates: unknown");
        check(idle_days(usage, "worn-hat", start) == 0, "a use in the future is no days ago");
    }

    // The file.
    {
        const auto folder = fs::temp_directory_path() / "dingosdk_mod_usage_tests";
        fs::remove_all(folder);
        fs::create_directories(folder);
        const auto file = folder / usage_file;
        check(read_usage(file).empty(), "no file: no usage");

        UsageLog log;
        log.seen("Team-Map", start);
        log.used("Team-Map", UseKind::map, start + day);
        log.used("Someone-Plugin", UseKind::plugin, start + 2 * day);
        log.used("Someone-Plugin", UseKind::music, start + 3 * day);
        log.used("Someone-Plugin", UseKind::object, start + 4 * day);
        log.used("Someone-Hat", UseKind::item, start + 5 * day);
        write_usage(file, log.usage());
        check(read_usage(file) == log.usage(), "usage reads back as written");
        check(!fs::exists(fs::path(file) += ".partial"), "nothing is left beside the file");

        write_text(file, R"({"schema":1,"mods":{
            "good": {"first_seen": 100, "map": {"last": 200, "count": 2}, "song": {"last": 1, "count": 1}},
            "bad-time": {"first_seen": "yesterday"},
            "bad-use": {"first_seen": 100, "item": {"last": 5, "count": -1}},
            "list": [1]
        }})");
        const auto damaged = read_usage(file);
        check(damaged.contains("good") && damaged.at("good").uses.size() == 1, "a kind it does not know is left out");
        check(damaged.at("good").uses.at(UseKind::map) == Use{200, 2}, "a good use is read");
        check(!damaged.contains("bad-time") && !damaged.contains("list"), "an entry of the wrong shape is dropped");
        check(damaged.contains("bad-use") && damaged.at("bad-use").uses.empty(), "a use of the wrong shape is dropped, the mod kept");

        write_text(file, R"({"schema":9,"mods":{}})");
        check(read_usage(file).empty(), "a later schema is not guessed at");
        write_text(file, "[");
        check(read_usage(file).empty(), "a broken file is no usage");
        fs::remove_all(folder);
    }

    if (failures) {
        std::cerr << failures << " mod usage check(s) failed\n";
        return 1;
    }
    std::cout << "mod usage checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
