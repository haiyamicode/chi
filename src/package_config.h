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

struct CInteropConfig {
    bool enabled = false;
    std::vector<std::string> include_directories;
    std::vector<std::string> source_files;
    std::vector<std::string> link_libraries;

    // Validate the configuration
    bool validate(std::string &error_message) const;
};

struct PackageConfig {
    std::string entry_file;
    std::optional<CInteropConfig> c_interop;

    // Validate the configuration using custom validation
    bool validate(std::string &error_message) const;

    // Validate against JSON schema
    static bool validate_with_schema(const boost::json::value &json, std::string &error_message);
};

// Boost.JSON serialization support for CInteropConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const CInteropConfig &config);
CInteropConfig tag_invoke(boost::json::value_to_tag<CInteropConfig>, const boost::json::value &jv);

// Boost.JSON serialization support for PackageConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const PackageConfig &config);
PackageConfig tag_invoke(boost::json::value_to_tag<PackageConfig>, const boost::json::value &jv);

} // namespace cx