#include "./toolchain.hpp"

#include <dds/toolchain/from_dds.hpp>
#include <dds/toolchain/prep.hpp>
#include <dds/util/algo.hpp>
#include <dds/util/string.hpp>

#include <cassert>
#include <optional>
#include <string>
#include <vector>

using namespace dds;

using std::optional;
using std::string;
using std::string_view;
using std::vector;
using opt_string = optional<string>;

toolchain toolchain::realize(const toolchain_prep& prep) {
    toolchain ret;
    ret._c_compile           = prep.c_compile;
    ret._cxx_compile         = prep.cxx_compile;
    ret._inc_template        = prep.include_template;
    ret._extern_inc_template = prep.external_include_template;
    ret._def_template        = prep.define_template;
    ret._link_archive        = prep.link_archive;
    ret._link_exe            = prep.link_exe;
    ret._warning_flags       = prep.warning_flags;
    ret._archive_prefix      = prep.archive_prefix;
    ret._archive_suffix      = prep.archive_suffix;
    ret._object_prefix       = prep.object_prefix;
    ret._object_suffix       = prep.object_suffix;
    ret._exe_prefix          = prep.exe_prefix;
    ret._exe_suffix          = prep.exe_suffix;
    ret._deps_mode           = prep.deps_mode;
    return ret;
}

vector<string> toolchain::include_args(const fs::path& p) const noexcept {
    return replace(_inc_template, "<PATH>", p.string());
}

vector<string> toolchain::external_include_args(const fs::path& p) const noexcept {
    return replace(_extern_inc_template, "<PATH>", p.string());
}

vector<string> toolchain::definition_args(std::string_view s) const noexcept {
    return replace(_def_template, "<DEF>", s);
}

compile_command_info
toolchain::create_compile_command(const compile_file_spec& spec) const noexcept {
    vector<string> flags;

    using namespace std::literals;

    language lang = spec.lang;
    if (lang == language::automatic) {
        if (spec.source_path.extension() == ".c" || spec.source_path.extension() == ".C") {
            lang = language::c;
        } else {
            lang = language::cxx;
        }
    }

    auto& cmd_template = lang == language::c ? _c_compile : _cxx_compile;

    for (auto&& inc_dir : spec.include_dirs) {
        auto inc_args = include_args(inc_dir);
        extend(flags, inc_args);
    }

    for (auto&& ext_inc_dir : spec.external_include_dirs) {
        auto inc_args = external_include_args(ext_inc_dir);
        extend(flags, inc_args);
    }

    for (auto&& def : spec.definitions) {
        auto def_args = definition_args(def);
        extend(flags, def_args);
    }

    if (spec.enable_warnings) {
        extend(flags, _warning_flags);
    }

    std::optional<fs::path> gnu_depfile_path;

    if (_deps_mode == file_deps_mode::gnu) {
        gnu_depfile_path = spec.out_path;
        gnu_depfile_path->replace_extension(gnu_depfile_path->extension().string() + ".d");
        extend(flags,
               {"-MD"sv,
                "-MF"sv,
                std::string_view(gnu_depfile_path->string()),
                "-MT"sv,
                std::string_view(spec.out_path.string())});
    } else if (_deps_mode == file_deps_mode::msvc) {
        flags.push_back("/showIncludes");
    }

    vector<string> command;
    for (auto arg : cmd_template) {
        if (arg == "<FLAGS>") {
            extend(command, flags);
        } else {
            arg = replace(arg, "<IN>", spec.source_path.string());
            arg = replace(arg, "<OUT>", spec.out_path.string());
            command.push_back(arg);
        }
    }
    return {command, gnu_depfile_path};
}

vector<string> toolchain::create_archive_command(const archive_spec& spec) const noexcept {
    vector<string> cmd;
    for (auto& arg : _link_archive) {
        if (arg == "<IN>") {
            std::transform(spec.input_files.begin(),
                           spec.input_files.end(),
                           std::back_inserter(cmd),
                           [](auto&& p) { return p.string(); });
        } else {
            cmd.push_back(replace(arg, "<OUT>", spec.out_path.string()));
        }
    }
    return cmd;
}

vector<string> toolchain::create_link_executable_command(const link_exe_spec& spec) const noexcept {
    vector<string> cmd;
    for (auto& arg : _link_exe) {
        if (arg == "<IN>") {
            std::transform(spec.inputs.begin(),
                           spec.inputs.end(),
                           std::back_inserter(cmd),
                           [](auto&& p) { return p.string(); });
        } else {
            cmd.push_back(replace(arg, "<OUT>", spec.output.string()));
        }
    }
    return cmd;
}

std::optional<toolchain> toolchain::get_builtin(std::string_view tc_id) noexcept {
    using namespace std::literals;

    std::string tc_content;

    if (starts_with(tc_id, "debug:")) {
        tc_id = tc_id.substr("debug:"sv.length());
        tc_content += "Debug: True\n";
    }

    if (starts_with(tc_id, "ccache:")) {
        tc_id = tc_id.substr("ccache:"sv.length());
        tc_content += "Compiler-Launcher: ccache\n";
    }

#define CXX_VER_TAG(str, version)                                                                  \
    if (starts_with(tc_id, str)) {                                                                 \
        tc_id = tc_id.substr(std::string_view(str).length());                                      \
        tc_content += "C++-Version: "s + version + "\n";                                           \
    }                                                                                              \
    static_assert(true)

    CXX_VER_TAG("c++98:", "C++98");
    CXX_VER_TAG("c++03:", "C++03");
    CXX_VER_TAG("c++11:", "C++11");
    CXX_VER_TAG("c++14:", "C++14");
    CXX_VER_TAG("c++17:", "C++17");
    CXX_VER_TAG("c++20:", "C++20");

    struct compiler_info {
        string c;
        string cxx;
        string id;
    };

    auto opt_triple = [&]() -> std::optional<compiler_info> {
        if (starts_with(tc_id, "gcc") || starts_with(tc_id, "clang")) {
            const bool is_gcc   = starts_with(tc_id, "gcc");
            const bool is_clang = starts_with(tc_id, "clang");

            const auto [c_compiler_base, cxx_compiler_base, compiler_id] = [&]() -> compiler_info {
                if (is_gcc) {
                    return {"gcc", "g++", "GNU"};
                } else if (is_clang) {
                    return {"clang", "clang++", "Clang"};
                }
                assert(false && "Unreachable");
                std::terminate();
            }();

            const auto compiler_suffix = [&]() -> std::string {
                if (ends_with(tc_id, "-7")) {
                    return "-7";
                } else if (ends_with(tc_id, "-8")) {
                    return "-8";
                } else if (ends_with(tc_id, "-9")) {
                    return "-9";
                } else if (ends_with(tc_id, "-10")) {
                    return "-10";
                } else if (ends_with(tc_id, "-11")) {
                    return "-11";
                } else if (ends_with(tc_id, "-12")) {
                    return "-12";
                } else if (ends_with(tc_id, "-13")) {
                    return "-13";
                }
                return "";
            }();

            auto c_compiler_name = c_compiler_base + compiler_suffix;
            if (c_compiler_name != tc_id) {
                return std::nullopt;
            }
            auto cxx_compiler_name = cxx_compiler_base + compiler_suffix;
            return compiler_info{c_compiler_name, cxx_compiler_name, compiler_id};
        } else if (tc_id == "msvc") {
            return compiler_info{"cl.exe", "cl.exe", "MSVC"};
        } else {
            return std::nullopt;
        }
    }();

    if (!opt_triple) {
        return std::nullopt;
    }

    tc_content += "C-Compiler: "s + opt_triple->c + "\n";
    tc_content += "C++-Compiler: "s + opt_triple->cxx + "\n";
    tc_content += "Compiler-ID: " + opt_triple->id + "\n";
    return parse_toolchain_dds(tc_content);
}
