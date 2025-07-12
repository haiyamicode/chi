/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "package_config.h"
#include <fstream>
#include <sstream>
#include <valijson/adapters/boost_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

namespace cx {

// Boost.JSON serialization for CInteropConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const CInteropConfig &config) {
    jv = {{"enabled", config.enabled},
          {"include_directories", config.include_directories},
          {"source_files", config.source_files},
          {"link_libraries", config.link_libraries}};
}

CInteropConfig tag_invoke(boost::json::value_to_tag<CInteropConfig>, const boost::json::value &jv) {
    CInteropConfig config;

    if (!jv.is_object()) {
        return config;
    }

    const auto &obj = jv.as_object();

    // Parse enabled flag with default value
    if (obj.if_contains("enabled")) {
        config.enabled = boost::json::value_to<bool>(obj.at("enabled"));
    }

    // Parse include directories
    if (obj.if_contains("include_directories")) {
        config.include_directories =
            boost::json::value_to<std::vector<std::string>>(obj.at("include_directories"));
    }

    // Parse source files
    if (obj.if_contains("source_files")) {
        config.source_files =
            boost::json::value_to<std::vector<std::string>>(obj.at("source_files"));
    }

    // Parse link libraries
    if (obj.if_contains("link_libraries")) {
        config.link_libraries =
            boost::json::value_to<std::vector<std::string>>(obj.at("link_libraries"));
    }

    return config;
}

bool CInteropConfig::validate(std::string &error_message) const {
    std::ostringstream errors;
    bool valid = true;

    if (enabled) {
        // If C interop is enabled, we should have at least some source files
        if (source_files.empty()) {
            errors << "c_interop.source_files: At least one source file must be specified when C "
                      "interop is enabled\n";
            valid = false;
        }

        // Validate that source file patterns are not empty
        for (const auto &pattern : source_files) {
            if (pattern.empty()) {
                errors << "c_interop.source_files: Source file patterns cannot be empty\n";
                valid = false;
                break;
            }
        }

        // Validate that include directories are not empty if specified
        for (const auto &dir : include_directories) {
            if (dir.empty()) {
                errors
                    << "c_interop.include_directories: Include directory paths cannot be empty\n";
                valid = false;
                break;
            }
        }

        // Validate that library names are not empty if specified
        for (const auto &lib : link_libraries) {
            if (lib.empty()) {
                errors << "c_interop.link_libraries: Library names cannot be empty\n";
                valid = false;
                break;
            }
        }
    }

    error_message = errors.str();
    return valid;
}

// Boost.JSON serialization for PackageConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const PackageConfig &config) {
    jv = {{"entry_file", config.entry_file}};

    if (config.c_interop.has_value()) {
        jv.as_object()["c_interop"] = boost::json::value_from(config.c_interop.value());
    }
}

PackageConfig tag_invoke(boost::json::value_to_tag<PackageConfig>, const boost::json::value &jv) {
    PackageConfig config;

    if (!jv.is_object()) {
        return config;
    }

    const auto &obj = jv.as_object();

    // Parse entry file (required)
    if (obj.if_contains("entry_file")) {
        config.entry_file = boost::json::value_to<std::string>(obj.at("entry_file"));
    }

    // Parse C interop configuration (optional)
    if (obj.if_contains("c_interop")) {
        config.c_interop = boost::json::value_to<CInteropConfig>(obj.at("c_interop"));
    }

    return config;
}

bool PackageConfig::validate(std::string &error_message) const {
    std::ostringstream errors;
    bool valid = true;

    // Validate entry file
    if (entry_file.empty()) {
        errors << "entry_file: Entry file must be specified\n";
        valid = false;
    }

    // Validate C interop configuration if present
    if (c_interop.has_value()) {
        std::string c_interop_error;
        if (!c_interop->validate(c_interop_error)) {
            errors << c_interop_error;
            valid = false;
        }
    }

    error_message = errors.str();
    return valid;
}

bool PackageConfig::validate_with_schema(const boost::json::value &json,
                                         const boost::json::value &schema_json,
                                         std::string &error_message) {
    try {
        // Create valijson schema and validator
        valijson::Schema schema;
        valijson::SchemaParser parser;
        valijson::adapters::BoostJsonAdapter schema_adapter(schema_json);
        parser.populateSchema(schema_adapter, schema);

        // Validate the JSON against the schema
        valijson::Validator validator;
        valijson::ValidationResults results;
        valijson::adapters::BoostJsonAdapter target_adapter(json);

        if (!validator.validate(schema, target_adapter, &results)) {
            std::ostringstream error_stream;
            error_stream << "Schema validation failed:\n";

            valijson::ValidationResults::Error error;
            unsigned int error_num = 1;
            while (results.popError(error)) {
                error_stream << "Error #" << error_num << ": " << error.description;
                for (const std::string &context : error.context) {
                    error_stream << " (at " << context << ")";
                }
                error_stream << "\n";
                ++error_num;
            }

            error_message = error_stream.str();
            return false;
        }

        return true;
    } catch (const std::exception &e) {
        error_message = "Schema validation exception: " + std::string(e.what());
        return false;
    }
}

} // namespace cx