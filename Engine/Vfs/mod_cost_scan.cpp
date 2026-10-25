#include "mod_cost_scan.h"
#include "mod_item_sources.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/texture.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

namespace dingosdk::mods {
using namespace detail;
namespace {
constexpr std::uint32_t mesh_set_type = 0x49b156d4;

// What a measurement reads through: the game's layout and archives, kept for every mod measured.
struct Reader {
    fs::path gameRoot, baseRoot;
    vfs::Layout layout;
    CasStore store;
    explicit Reader(const fs::path& dataRoot)
        : gameRoot(dataRoot), baseRoot(dataRoot / L"Data"), layout(vfs::read_layout(baseRoot / L"layout.toc")),
          store(baseRoot, dataRoot / mods_folder / generated_folder, layout.root) {}
};

void add_folder(ModCost& cost, const fs::path& directory) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) { error.clear(); continue; }
        const auto size = it->file_size(error);
        if (!error) cost.disk_bytes += size;
        ++cost.files;
        error.clear();
    }
}

// Every level of detail of a MeshSet: what its vertex and index buffers take (as the merge counts
// a map's meshes, mod_merge_combine.cpp).
std::uint64_t mesh_bytes(std::span<const std::byte> payload) {
    std::size_t lods{};
    const auto first = fb::any_mesh_lod_geometry(payload, 0, &lods);
    std::uint64_t bytes = std::uint64_t{first.vertex_bytes} + first.index_bytes;
    for (std::size_t lod = 1; lod < lods; ++lod) {
        const auto geometry = fb::any_mesh_lod_geometry(payload, lod);
        bytes += std::uint64_t{geometry.vertex_bytes} + geometry.index_bytes;
    }
    return bytes;
}

ModCost measure(const Mod& mod, const Reader& reader) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    ModCost cost;
    cost.fingerprint = mod_fingerprint(mod.directory);
    add_folder(cost, mod.directory);
    // Each payload of the mod's once, though several of its bundles may list it.
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint32_t>> counted;
    for (const auto& relative : scan(mod.directory).tocs) {
        fb::TocDocument toc;
        try {
            toc = fb::read_toc(read_file(mod.directory / fs::path(relative)));
        } catch (const std::exception&) {
            continue;   // (the merge says what it cannot read)
        }
        for (const auto& bundle : toc.bundles) {
            try {
                const auto listing = list_bundle(reader.store, mod.directory, reader.baseRoot, bundle, reader.gameRoot);
                if (!listing) continue;
                const auto& manifest = listing->manifest;
                for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                    const auto& asset = manifest.resources[index];
                    const bool texture = asset.resourceType == fb::texture_resource_type;
                    if (!texture && asset.resourceType != mesh_set_type) continue;
                    const auto at = listing->first + manifest.ebx.size() + index;
                    if (at >= listing->files.size()) continue;
                    const auto& file = listing->files[at];
                    if (!file.location.patch) continue;   // the game's own
                    if (!counted.emplace(file.location.installChunk, file.location.archive, file.offset).second) continue;
                    try {
                        const auto payload = fb::decode_cas(reader.store.read(mod.directory, file.location, file.offset, file.size),
                                                            {reader.gameRoot});
                        if (texture) add_texture(cost, fb::read_texture_header(payload, asset.resourceMeta));
                        else cost.mesh_bytes += mesh_bytes(payload);
                    } catch (const std::exception&) {
                        // A header of a layout not read here counts for nothing.
                    }
                }
            } catch (const std::exception&) {}
        }
    }
    cost.read_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    return cost;
}

// `count` pieces of work on up to `threads` threads beside this one, taken in turn (as the conflict
// check does it, mod_merge_conflicts.cpp).
void in_parallel(std::size_t count, std::size_t threads, bool background, const std::function<void(std::size_t)>& work) {
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (;;) {
            const auto mine = next.fetch_add(1);
            if (mine >= count) return;
            try {
                work(mine);
            } catch (const std::exception&) {}
        }
    };
    std::vector<std::jthread> workers;
    try {
        for (std::size_t index = 0; index + 1 < count && index < threads; ++index)
            workers.emplace_back([&] {
                if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                run();
            });
    } catch (const std::system_error&) {}
    run();
}
} // namespace

ModCost measure_cost(const Mod& mod, const fs::path& data_root) {
    try {
        const Reader reader(data_root);
        auto cost = measure(mod, reader);
        reader.store.close_all();
        return cost;
    } catch (const std::exception&) {
        ModCost cost;   // sizes only, with no fingerprint: not taken for a measurement
        add_folder(cost, mod.directory);
        return cost;
    }
}

Costs refresh_costs(const fs::path& data_root, const std::vector<const Mod*>& mods, std::size_t threads, bool background,
                    const std::atomic<bool>* stop) noexcept {
    const auto modsRoot = data_root / mods_folder;
    const auto file = modsRoot / costs_file;
    Costs before;
    try {
        before = read_costs(file);
        // What is still to measure: the mods whose files are not the ones measured.
        std::vector<std::string> fingerprints(mods.size());
        in_parallel(mods.size(), threads, background, [&](std::size_t at) { fingerprints[at] = mod_fingerprint(mods[at]->directory); });
        Costs costs;
        std::vector<std::size_t> unmeasured;
        for (std::size_t at = 0; at < mods.size(); ++at) {
            if (const auto* known = known_cost(before, mods[at]->name, fingerprints[at])) costs.emplace(mods[at]->name, *known);
            else unmeasured.push_back(at);
        }
        if (!unmeasured.empty()) {
            std::optional<Reader> reader;
            try {
                reader.emplace(data_root);
            } catch (const std::exception&) {}   // without the game's layout, only sizes on disk
            std::mutex mutex;
            in_parallel(unmeasured.size(), threads, background, [&](std::size_t index) {
                if (stop && *stop) return;
                const auto& mod = *mods[unmeasured[index]];
                ModCost cost;
                if (reader) {
                    cost = measure(mod, *reader);
                } else {
                    // Sizes only, with no fingerprint: measured in full the next time.
                    add_folder(cost, mod.directory);
                }
                std::lock_guard lock(mutex);
                costs.emplace(mod.name, std::move(cost));
            });
            // What was read is kept, the files are not: a mod's archives must be free to be replaced.
            if (reader) reader->store.close_all();
        }
        // The items each mod adds or changes, as the last merge found them.
        const auto sources = read_item_sources(modsRoot / generated_folder / item_sources_file);
        for (auto& [name, cost] : costs) {
            const auto source = std::ranges::find(sources, name, &ItemSource::mod);
            cost.items = source == sources.end() ? 0 : source->keys.size();
        }
        if (costs != before) write_costs(file, costs);
        return costs;
    } catch (...) {
        return before;
    }
}

} // namespace dingosdk::mods
