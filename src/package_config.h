/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <optional>
#include <string>
#include <vector>
#define BOOST_JSON_STANDALONE
#include "include/boost/json.hpp"
#include <valijson/adapters/boost_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

namespace cx {

struct NativeModuleConfig {
    std::string name;                       // Module name (e.g., "C", "SDL")
    std::vector<std::string> includes;      // Header files to parse
    std::vector<std::string> symbols;       // Filter: only import these symbols
    std::vector<std::string> include_paths; // Additional include directories
    std::vector<std::string> link;          // Libraries to link
    std::string pkg_config;                 // Optional: use pkg-config
};

void tag_invoke(boost::json::value_from_tag, boost::json::value &jv,
                const NativeModuleConfig &config);
NativeModuleConfig tag_invoke(boost::json::value_to_tag<NativeModuleConfig>,
                              const boost::json::value &jv);

struct CInteropConfig {
    bool enabled = false;
    std::vector<std::string> include_directories;
    std::vector<std::string> source_files;
    std::vector<std::string> link_libraries;
    std::vector<std::string> library_paths;

    // New: native modules using libclang
    std::map<std::string, NativeModuleConfig> native_modules;
};

void tag_invoke(boost::json::value_from_tag, boost::json::value &jv,
                const CInteropConfig &config);
CInteropConfig tag_invoke(boost::json::value_to_tag<CInteropConfig>,
                          const boost::json::value &jv);

struct PackageConfig {
    std::optional<std::string> name;
    std::optional<std::string> version;
    std::optional<std::string> description;
    std::vector<std::string> include;
    std::string entry_file;
    std::optional<CInteropConfig> c_interop;

    // Validate against JSON schema
    static bool validate_with_schema(const boost::json::value &json,
                                     const boost::json::value &schema, std::string &error_message);
};

void tag_invoke(boost::json::value_from_tag, boost::json::value &jv,
                const PackageConfig &config);
PackageConfig tag_invoke(boost::json::value_to_tag<PackageConfig>,
                         const boost::json::value &jv);

} // namespace cx
