#include "shield/commands/migrate_command.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace shield::commands {

namespace {

std::string now_timestamp_compact() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm)) {
        return std::string(buf);
    }
    return "unknown-time";
}

YAML::Node build_log_from_legacy_logger(const YAML::Node& logger) {
    YAML::Node log;

    std::string level = "info";
    if (logger["level"] && logger["level"].IsScalar()) {
        level = logger["level"].as<std::string>();
    }

    log["global_level"] = level;

    YAML::Node console;
    console["enabled"] = logger["console_output"]
                             ? logger["console_output"].as<bool>()
                             : true;
    console["colored"] = true;
    console["pattern"] =
        logger["pattern"]
            ? logger["pattern"].as<std::string>()
            : "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
    console["min_level"] = level;
    log["console"] = console;

    YAML::Node file;
    bool enabled = false;
    if (logger["file_output"]) {
        enabled = logger["file_output"].as<bool>();
    } else if (logger["log_file"] || logger["file_path"]) {
        enabled = true;
    }
    file["enabled"] = enabled;

    if (logger["log_file"]) {
        file["log_file"] = logger["log_file"].as<std::string>();
    } else if (logger["file_path"]) {
        file["log_file"] = logger["file_path"].as<std::string>();
    } else {
        file["log_file"] = "logs/shield.log";
    }

    if (logger["max_file_size"]) {
        file["max_file_size"] = logger["max_file_size"].as<std::int64_t>();
    }
    if (logger["max_files"]) {
        file["max_files"] = logger["max_files"].as<int>();
    }

    file["rotate_on_open"] = false;
    file["pattern"] =
        logger["pattern"]
            ? logger["pattern"].as<std::string>()
            : "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
    file["min_level"] = level;
    log["file"] = file;

    return log;
}

void migrate_log_config(YAML::Node& root, std::vector<std::string>& actions,
                        bool drop_legacy) {
    if (!root || !root.IsMap()) {
        YAML::Node normalized(YAML::NodeType::Map);
        root = normalized;
        actions.push_back("Normalized root node to a map");
    }

    YAML::Node legacy_logger = root["logger"];
    if (legacy_logger && legacy_logger.IsMap()) {
        if (!root["log"] || root["log"].IsNull()) {
            root["log"] = build_log_from_legacy_logger(legacy_logger);
            actions.push_back("Created `log` from legacy `logger`");
        } else if (root["log"].IsMap()) {
            auto log = root["log"];
            if (!log["global_level"] && legacy_logger["level"]) {
                log["global_level"] = legacy_logger["level"].as<std::string>();
                actions.push_back(
                    "Filled `log.global_level` from `logger.level`");
            }
            if (!log["console"] && legacy_logger["console_output"]) {
                YAML::Node console;
                console["enabled"] = legacy_logger["console_output"].as<bool>();
                console["colored"] = true;
                console["pattern"] =
                    legacy_logger["pattern"]
                        ? legacy_logger["pattern"].as<std::string>()
                        : "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
                console["min_level"] = log["global_level"]
                                           ? log["global_level"].as<std::string>()
                                           : "info";
                log["console"] = console;
                actions.push_back("Filled `log.console` from legacy `logger`");
            }
            if (!log["file"] &&
                (legacy_logger["file_output"] || legacy_logger["file_path"] ||
                 legacy_logger["log_file"])) {
                YAML::Node file;
                const bool enabled = legacy_logger["file_output"]
                                         ? legacy_logger["file_output"].as<bool>()
                                         : true;
                file["enabled"] = enabled;
                if (legacy_logger["log_file"]) {
                    file["log_file"] = legacy_logger["log_file"].as<std::string>();
                } else if (legacy_logger["file_path"]) {
                    file["log_file"] = legacy_logger["file_path"].as<std::string>();
                }
                if (legacy_logger["max_file_size"]) {
                    file["max_file_size"] =
                        legacy_logger["max_file_size"].as<std::int64_t>();
                }
                if (legacy_logger["max_files"]) {
                    file["max_files"] = legacy_logger["max_files"].as<int>();
                }
                file["rotate_on_open"] = false;
                file["pattern"] =
                    legacy_logger["pattern"]
                        ? legacy_logger["pattern"].as<std::string>()
                        : "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
                file["min_level"] = log["global_level"]
                                        ? log["global_level"].as<std::string>()
                                        : "info";
                log["file"] = file;
                actions.push_back("Filled `log.file` from legacy `logger`");
            }
            root["log"] = log;
        }

        if (drop_legacy) {
            root.remove("logger");
            actions.push_back("Removed legacy `logger` section");
        }
    }

    YAML::Node log = root["log"];
    if (!log || !log.IsMap()) return;

    if (!log["global_level"] && log["level"]) {
        log["global_level"] = log["level"];
        if (drop_legacy) {
            log.remove("level");
            actions.push_back("Replaced `log.level` with `log.global_level`");
        } else {
            actions.push_back("Added `log.global_level` from `log.level`");
        }
    }

    if (log["file"] && log["file"].IsScalar()) {
        const auto file_path = log["file"].as<std::string>();
        YAML::Node file;
        file["enabled"] = true;
        file["log_file"] = file_path;
        file["max_file_size"] = 10485760;
        file["max_files"] = 5;
        file["rotate_on_open"] = false;
        file["pattern"] = "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
        file["min_level"] = log["global_level"]
                                ? log["global_level"].as<std::string>()
                                : "info";
        log["file"] = file;
        actions.push_back(
            "Converted shorthand `log.file: <path>` to structured `log.file`");
    }

    if (!log["console"] && log["console_output"]) {
        YAML::Node console;
        console["enabled"] = log["console_output"].as<bool>();
        console["colored"] = true;
        console["pattern"] = log["pattern"] ? log["pattern"].as<std::string>()
                                            : "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
        console["min_level"] = log["global_level"]
                                   ? log["global_level"].as<std::string>()
                                   : "info";
        log["console"] = console;
        if (drop_legacy) {
            log.remove("console_output");
            actions.push_back(
                "Converted `log.console_output` to `log.console.enabled` (dropped legacy key)");
        } else {
            actions.push_back(
                "Converted `log.console_output` to `log.console.enabled`");
        }
    }

    if (drop_legacy && log["pattern"]) {
        log.remove("pattern");
        actions.push_back("Removed legacy `log.pattern` key");
    }

    root["log"] = log;
}

std::string emit_yaml(const YAML::Node& root) {
    YAML::Emitter out;
    out.SetIndent(2);
    out << root;
    if (!out.good()) {
        throw std::runtime_error(std::string("YAML emit failed: ") +
                                 out.GetLastError());
    }
    return std::string(out.c_str());
}

}  // namespace

MigrateCommand::MigrateCommand()
    : shield::cli::Command("migrate",
                           "Database and configuration migration tool") {
    setup_flags();
    set_long_description(
        "Migrate data and configuration between Shield versions.")
        .set_usage("shield migrate [OPTIONS]")
        .set_example(
            "  shield migrate --from v1.0 --to v2.0\\n"
            "  shield migrate --dry-run --backup\\n"
            "  shield migrate --input config/app.yaml --output config/app.yaml "
            "--backup\\n"
            "  shield migrate --in-place --drop-legacy");
}

void MigrateCommand::setup_flags() {
    add_flag_with_short("input", "i", "Input configuration file", "");
    add_flag_with_short("output", "o", "Output configuration file", "");
    add_bool_flag("in-place", "Rewrite input file in place", false);
    add_bool_flag("drop-legacy", "Remove legacy config keys/sections", false);

    add_flag("from", "Source version", "");
    add_flag("to", "Target version", "");
    add_bool_flag("dry-run",
                  "Show what would be migrated without making changes", false);
    add_bool_flag("backup", "Create backup before migration", true);
}

int MigrateCommand::run(shield::cli::CommandContext& ctx) {
    const bool dry_run = ctx.get_bool_flag("dry-run");
    const bool backup = ctx.get_bool_flag("backup");
    const bool in_place = ctx.get_bool_flag("in-place");
    const bool drop_legacy = ctx.get_bool_flag("drop-legacy");

    std::string input = ctx.get_flag("input");
    if (input.empty()) {
        input = ctx.get_flag("config");
    }
    if (input.empty()) {
        std::cerr << "No input provided (use --input or --config)" << std::endl;
        return 1;
    }

    std::string output = ctx.get_flag("output");
    if (in_place) {
        output = input;
    }

    if (!std::filesystem::exists(input)) {
        std::cerr << "Input file not found: " << input << std::endl;
        return 1;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(input);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse YAML: " << e.what() << std::endl;
        return 1;
    }

    std::vector<std::string> actions;
    try {
        migrate_log_config(root, actions, drop_legacy);
    } catch (const std::exception& e) {
        std::cerr << "Migration failed: " << e.what() << std::endl;
        return 1;
    }

    std::string migrated;
    try {
        migrated = emit_yaml(root);
    } catch (const std::exception& e) {
        std::cerr << "Failed to emit migrated YAML: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Shield Migration Tool\n";
    if (!ctx.get_flag("from").empty() || !ctx.get_flag("to").empty()) {
        std::cout << "From: " << ctx.get_flag("from") << "\n";
        std::cout << "To: " << ctx.get_flag("to") << "\n";
    }
    std::cout << "Input: " << input << "\n";
    if (!output.empty()) {
        std::cout << "Output: " << output << (dry_run ? " (dry-run)" : "")
                  << "\n";
    } else {
        std::cout << "Output: stdout" << (dry_run ? " (dry-run)" : "") << "\n";
    }

    if (actions.empty()) {
        std::cout << "No changes needed.\n";
    } else {
        std::cout << "Planned changes:\n";
        for (const auto& a : actions) {
            std::cout << " - " << a << "\n";
        }
    }

    if (dry_run || output.empty()) {
        std::cout << "\n---\n" << migrated << "\n";
        return 0;
    }

    if (backup && std::filesystem::exists(output)) {
        const auto backup_path = output + ".bak." + now_timestamp_compact();
        try {
            std::filesystem::copy_file(
                output, backup_path,
                std::filesystem::copy_options::overwrite_existing);
            std::cout << "Backup created: " << backup_path << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Failed to create backup: " << e.what() << std::endl;
            return 1;
        }
    }

    try {
        std::ofstream ofs(output, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "Failed to open output file: " << output << std::endl;
            return 1;
        }
        ofs << migrated << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Failed to write output file: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Migration complete.\n";
    return 0;
}

}  // namespace shield::commands
