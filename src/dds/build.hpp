#ifndef DDS_BUILD_HPP_INCLUDED
#define DDS_BUILD_HPP_INCLUDED

#include <dds/manifest.hpp>
#include <dds/util.hpp>

#include <optional>

namespace dds {

struct build_params {
    fs::path    root;
    fs::path    out_root;
    fs::path    toolchain_file;
    std::string export_name;
    bool        do_export       = false;
    bool        build_tests     = false;
    bool        enable_warnings = false;
    int         parallel_jobs   = 0;
};

void build(const build_params&, const library_manifest& man);

}  // namespace dds

#endif  // DDS_BUILD_HPP_INCLUDED