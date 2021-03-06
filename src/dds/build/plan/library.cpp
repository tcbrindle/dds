#include "./library.hpp"

#include <dds/util/algo.hpp>

#include <range/v3/view/concat.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/transform.hpp>

#include <cassert>

using namespace dds;

library_plan library_plan::create(const library& lib, const library_build_params& params) {
    // Source files are kept in three groups:
    std::vector<source_file> app_sources;
    std::vector<source_file> test_sources;
    std::vector<source_file> lib_sources;

    // Collect the source for this library. This will look for any compilable sources in the `src/`
    // subdirectory of the library.
    auto src_dir = lib.src_dir();
    if (src_dir.exists()) {
        // Sort each source file between the three source arrays, depending on
        // the kind of source that we are looking at.
        auto all_sources = src_dir.collect_sources();
        for (const auto& sfile : all_sources) {
            if (sfile.kind == source_kind::test) {
                test_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::app) {
                app_sources.push_back(sfile);
            } else if (sfile.kind == source_kind::source) {
                lib_sources.push_back(sfile);
            } else {
                assert(sfile.kind == source_kind::header);
            }
        }
    }

    // Load up the compile rules
    auto compile_rules              = lib.base_compile_rules();
    compile_rules.enable_warnings() = params.enable_warnings;
    compile_rules.uses()            = lib.manifest().uses;

    // Convert the library sources into their respective file compilation plans.
    auto lib_compile_files =  //
        lib_sources           //
        | ranges::views::transform([&](const source_file& sf) {
              return compile_file_plan(compile_rules,
                                       sf,
                                       lib.manifest().name,
                                       params.out_subdir / "obj");
          })
        | ranges::to_vector;

    // If we have any compiled library files, generate a static library archive
    // for this library
    std::optional<create_archive_plan> create_archive;
    if (!lib_compile_files.empty()) {
        create_archive.emplace(lib.manifest().name,
                               params.out_subdir,
                               std::move(lib_compile_files));
    }

    // Collect the paths to linker inputs that should be used when generating executables for this
    // library.
    std::vector<lm::usage> links;
    extend(links, lib.manifest().uses);
    extend(links, lib.manifest().links);

    // Linker inputs for tests may contain additional code for test execution
    std::vector<fs::path> link_libs;
    std::vector<fs::path> test_link_libs = params.test_link_files;

    // There may also be additional usage requirements for tests
    auto test_rules = compile_rules.clone();
    auto test_links = links;
    extend(test_rules.uses(), params.test_uses);
    extend(test_links, params.test_uses);

    // Generate the plans to link any executables for this library
    std::vector<link_executable_plan> link_executables;
    for (const source_file& source : ranges::views::concat(app_sources, test_sources)) {
        const bool is_test = source.kind == source_kind::test;
        // Pick a subdir based on app/test
        const auto subdir_base = is_test ? params.out_subdir / "test" : params.out_subdir;
        // Put test/app executables in a further subdirectory based on the source file path
        const auto subdir
            = subdir_base / fs::relative(source.path.parent_path(), lib.src_dir().path);
        // Pick compile rules based on app/test
        auto rules = is_test ? test_rules : compile_rules;
        // Pick input libs based on app/test
        auto& exe_link_libs = is_test ? test_link_libs : link_libs;
        auto& exe_links     = is_test ? test_links : links;
        // TODO: Apps/tests should only see the _public_ include dir, not both
        auto exe = link_executable_plan{exe_link_libs,
                                        exe_links,
                                        compile_file_plan(rules,
                                                          source,
                                                          lib.manifest().name,
                                                          params.out_subdir / "obj"),
                                        subdir,
                                        source.path.stem().stem().string()};
        link_executables.emplace_back(std::move(exe));
    }

    // Done!
    return library_plan{lib, std::move(create_archive), std::move(link_executables)};
}
