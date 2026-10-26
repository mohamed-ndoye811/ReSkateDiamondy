#include "mod_usage_tracker.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Boot/exit_watch.h"
#include "Extension/Customization/item_origins.h"
#include "Extension/Customization/worn_items.h"
#include "Extension/World/level_loading.h"
#include <Windows.h>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::mod_usage_tracker {
namespace {
constexpr std::uint64_t look_every_ms = 30'000;
constexpr std::uint64_t write_every_ms = 5 * 60'000;

struct Tracker {
    std::mutex mutex;
    std::optional<mods::UsageLog> log;   // read on first use; under mutex
    std::filesystem::path file;
    bool write_failed{};                 // said once
    std::uint64_t next_look{}, next_write{};   // client thread only
};
Tracker& tracker() { static auto* value = new Tracker; return *value; }

std::int64_t now_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The folder a use counts for: a variant's is its mod's, as mods.json and the launcher know it.
std::string folder_of(std::string_view mod) {
    auto owner = mods::variant_owner(mod);
    return owner.empty() ? std::string(mod) : owner;
}

// The log, read from the Mods folder the first time. Null when there is no Mods folder. Under mutex.
mods::UsageLog* opened(Tracker& t) {
    if (t.log) return &*t.log;
    const auto& catalog = mods::catalog();
    if (!catalog.present) return nullptr;
    t.file = catalog.root / mods::usage_file;
    t.log.emplace(mods::read_usage(t.file));
    return &*t.log;
}

void write_out(Tracker& t) noexcept {
    try {
        if (!t.log || !t.log->changed()) return;
        mods::write_usage(t.file, t.log->usage());
        t.log->mark_written();
    } catch (const std::exception& error) {
        if (!std::exchange(t.write_failed, true))
            logging::log(logging::Level::warning, logging::Channel::assets, "Mod usage could not be saved: {}", error.what());
    }
}

void write_on_close() noexcept {
    auto& t = tracker();
    std::lock_guard lock(t.mutex);
    write_out(t);
}

// The enabled mods, the level loaded and what the skater wears now. Client thread.
void look(Tracker& t) {
    const auto& catalog = mods::catalog();
    const auto now = now_seconds();
    std::string map;
    if (level_content_active())
        if (const auto* mod = mods::mod_registering(catalog, last_level_destination())) map = folder_of(mod->name);
    std::vector<std::string> worn;
    for (const auto& list : {worn_items::worn_outfit(), worn_items::worn_board()})
        for (const auto& item : list)
            if (const auto origin = item_origins::find(item.item); origin && !origin->mod.empty()) worn.push_back(folder_of(origin->mod));

    std::lock_guard lock(t.mutex);
    auto* log = opened(t);
    if (!log) return;
    for (const auto& mod : catalog.mods) log->seen(folder_of(mod.name), now);
    if (!map.empty()) log->used(map, mods::UseKind::map, now);
    for (const auto& mod : worn) log->used(mod, mods::UseKind::item, now);
}
} // namespace

void note(std::string_view mod, mods::UseKind kind) noexcept {
    try {
        if (mod.empty()) return;
        auto& t = tracker();
        std::lock_guard lock(t.mutex);
        if (auto* log = opened(t)) log->used(folder_of(mod), kind, now_seconds());
    } catch (...) {}
}

void poll() noexcept {
    const auto error = GetLastError();
    try {
        auto& t = tracker();
        const auto now = GetTickCount64();
        if (now >= t.next_look) {
            if (!t.next_look) {
                exit_watch::when_closing(&write_on_close);
                t.next_write = now + write_every_ms;
            }
            t.next_look = now + look_every_ms;
            look(t);
        }
        if (now >= t.next_write) {
            t.next_write = now + write_every_ms;
            std::lock_guard lock(t.mutex);
            write_out(t);
        }
    } catch (...) {}
    SetLastError(error);
}
} // namespace dingosdk::mod_usage_tracker
