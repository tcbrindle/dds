#include "./repo.hpp"

#include <dds/catalog/catalog.hpp>
#include <dds/error/errors.hpp>
#include <dds/solve/solve.hpp>
#include <dds/source/dist.hpp>
#include <dds/util/paths.hpp>
#include <dds/util/string.hpp>

#include <spdlog/spdlog.h>

#include <range/v3/action/sort.hpp>
#include <range/v3/action/unique.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/transform.hpp>

using namespace dds;

using namespace ranges;

namespace {

auto load_sdists(path_ref root) {
    using namespace ranges;
    using namespace ranges::views;

    auto try_read_sdist = [](path_ref p) -> std::optional<sdist> {
        if (starts_with(p.filename().string(), ".")) {
            return std::nullopt;
        }
        try {
            return sdist::from_directory(p);
        } catch (const std::runtime_error& e) {
            spdlog::error("Failed to load source distribution from directory '{}': {}",
                          p.string(),
                          e.what());
            return std::nullopt;
        }
    };

    return
        // Get the top-level `name-version` dirs
        fs::directory_iterator(root)  //
        // // Convert each dir into an `sdist` object
        | transform(try_read_sdist)  //
        // // Drop items that failed to load
        | filter([](auto&& opt) { return opt.has_value(); })  //
        | transform([](auto&& opt) { return *opt; })          //
        ;
}

}  // namespace

void repository::_log_blocking(path_ref dirpath) noexcept {
    spdlog::warn("Another process has the repository directory locked [{}]", dirpath.string());
    spdlog::warn("Waiting for repository to be released...");
}

void repository::_init_repo_dir(path_ref dirpath) noexcept { fs::create_directories(dirpath); }

fs::path repository::default_local_path() noexcept { return dds_data_dir() / "repo"; }

repository repository::_open_for_directory(bool writeable, path_ref dirpath) {
    sdist_set entries = load_sdists(dirpath) | to<sdist_set>();
    return {writeable, dirpath, std::move(entries)};
}

void repository::add_sdist(const sdist& sd, if_exists ife_action) {
    if (!_write_enabled) {
        spdlog::critical(
            "DDS attempted to write into a repository that wasn't opened with a write-lock. This "
            "is a hard bug and should be reported. For the safety and integrity of the local "
            "repository, we'll hard-exit immediately.");
        std::terminate();
    }
    auto sd_dest = _root / sd.manifest.pkg_id.to_string();
    if (fs::exists(sd_dest)) {
        auto msg = fmt::format("Source distribution '{}' is already available in the local repo",
                               sd.path.string());
        if (ife_action == if_exists::throw_exc) {
            throw_user_error<errc::sdist_exists>(msg);
        } else if (ife_action == if_exists::ignore) {
            spdlog::warn(msg);
            return;
        } else {
            spdlog::info(msg + " - Replacing");
        }
    }
    auto tmp_copy = sd_dest;
    tmp_copy.replace_filename(".tmp-import");
    if (fs::exists(tmp_copy)) {
        fs::remove_all(tmp_copy);
    }
    fs::create_directories(tmp_copy.parent_path());
    fs::copy(sd.path, tmp_copy, fs::copy_options::recursive);
    if (fs::exists(sd_dest)) {
        fs::remove_all(sd_dest);
    }
    fs::rename(tmp_copy, sd_dest);
    _sdists.insert(sdist::from_directory(sd_dest));
    spdlog::info("Source distribution '{}' successfully exported", sd.manifest.pkg_id.to_string());
}

const sdist* repository::find(const package_id& pkg) const noexcept {
    auto found = _sdists.find(pkg);
    if (found == _sdists.end()) {
        return nullptr;
    }
    return &*found;
}

std::vector<package_id> repository::solve(const std::vector<dependency>& deps,
                                          const catalog&                 ctlg) const {
    return dds::solve(
        deps,
        [&](std::string_view name) -> std::vector<package_id> {
            auto mine = ranges::views::all(_sdists)  //
                | ranges::views::filter(
                            [&](const sdist& sd) { return sd.manifest.pkg_id.name == name; })
                | ranges::views::transform([](const sdist& sd) { return sd.manifest.pkg_id; });
            auto avail = ctlg.by_name(name);
            auto all   = ranges::views::concat(mine, avail) | ranges::to_vector;
            ranges::sort(all, std::less<>{});
            ranges::unique(all, std::less<>{});
            return all;
        },
        [&](const package_id& pkg_id) {
            auto found = find(pkg_id);
            if (found) {
                return found->manifest.dependencies;
            }
            return ctlg.dependencies_of(pkg_id);
        });
}
