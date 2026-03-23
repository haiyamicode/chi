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

// Boost.JSON serialization for NativeModuleConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv,
                const NativeModuleConfig &config) {
    boost::json::object obj;
    obj["name"] = config.name;
    obj["includes"] = boost::json::value_from(config.includes);
    obj["symbols"] = boost::json::value_from(config.symbols);

    if (!config.include_paths.empty()) {
        obj["include_paths"] = boost::json::value_from(config.include_paths);
    }
    if (!config.link.empty()) {
        obj["link"] = boost::json::value_from(config.link);
    }
    if (!config.pkg_config.empty()) {
        obj["pkg_config"] = config.pkg_config;
    }

    jv = obj;
}

NativeModuleConfig tag_invoke(boost::json::value_to_tag<NativeModuleConfig>,
                              const boost::json::value &jv) {
    NativeModuleConfig config;

    if (!jv.is_object()) {
        return config;
    }

    const auto &obj = jv.as_object();

    if (obj.if_contains("includes")) {
        config.includes = boost::json::value_to<std::vector<std::string>>(obj.at("includes"));
    }

    if (obj.if_contains("symbols")) {
        config.symbols = boost::json::value_to<std::vector<std::string>>(obj.at("symbols"));
    }

    if (obj.if_contains("include_paths")) {
        config.include_paths =
            boost::json::value_to<std::vector<std::string>>(obj.at("include_paths"));
    }

    if (obj.if_contains("link")) {
        config.link = boost::json::value_to<std::vector<std::string>>(obj.at("link"));
    }

    if (obj.if_contains("pkg_config")) {
        config.pkg_config = boost::json::value_to<std::string>(obj.at("pkg_config"));
    }

    return config;
}

// Boost.JSON serialization for CInteropConfig
void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const CInteropConfig &config) {
    jv = {{"enabled", config.enabled},
          {"include_directories", config.include_directories},
          {"source_files", config.source_files},
          {"link_libraries", config.link_libraries},
          {"library_paths", config.library_paths}};
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

    // Parse library paths
    if (obj.if_contains("library_paths")) {
        config.library_paths =
            boost::json::value_to<std::vector<std::string>>(obj.at("library_paths"));
    }

    // Parse native modules (new libclang-based approach)
    if (obj.if_contains("native_modules")) {
        const auto &modules_obj = obj.at("native_modules").as_object();
        for (const auto &[module_name, module_config] : modules_obj) {
            NativeModuleConfig nm_config = boost::json::value_to<NativeModuleConfig>(module_config);
            std::string name_str(module_name);
            nm_config.name = name_str;
            config.native_modules[name_str] = nm_config;
        }
    }

    return config;
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

bool PackageConfig::validate_with_schema(const boost::json::value &json,
                                         const boost::json::value &schema_json,
                                         std::string &error_message) {
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
}

} // namespace cx
