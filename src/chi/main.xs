import "std/args" as args;
import "std/filepath" as filepath;
import "std/fs" as fs;
import "std/json" as json;
import "std/os" as os;

struct CliError {
    detail: string = "";

    impl Error {
        func message() string {
            return this.detail;
        }
    }
}

func cli_error(message: string) never {
    throw new CliError{detail: move message};
}

func find_in_dir(dir: string, name: string) ?string {
    if dir.is_empty() {
        return null;
    }
    let candidate = filepath.join(resolve_search_path(dir), name);
    if fs.exists(candidate) {
        return candidate;
    }
    return null;
}

func find_in_path(path_value: string, name: string) ?string {
    if path_value.is_empty() {
        return null;
    }
    let entries = path_value.split(":");
    for entry in entries {
        if let candidate = find_in_dir(entry, name) {
            return candidate;
        }
    }
    return null;
}

func find_chic() string {
    if let chi_path = os.env("CHI_PATH") {
        if let candidate = find_in_dir(chi_path, "chic") {
            return candidate;
        }
    }

    if let chi_root = os.env("CHI_ROOT") {
        if let candidate = find_in_dir(filepath.join(chi_root, "bin"), "chic") {
            return candidate;
        }
    }

    if let path_value = os.env("PATH") {
        if let candidate = find_in_path(path_value, "chic") {
            return candidate;
        }
    }

    cli_error("could not find Chi compiler binary (chic)");
}

func resolve_search_path(input: string) string {
    if filepath.is_absolute(input) {
        return input;
    }
    return filepath.join(os.cwd(), input);
}

func find_package_root(start: string) ?string {
    var current = start;
    while true {
        let config_path = filepath.join(current, "package.jsonc");
        if fs.exists(config_path) {
            return current;
        }
        let parent = filepath.dir(current);
        if parent == current {
            return null;
        }
        current = parent;
    }
}

func package_command(name: string, summary: string, include_output: bool) args.Command {
    var command = args.Command{
        :name,
        :summary
    };
    if include_output {
        command.option({
            name: "output",
            short: "o",
            value_name: "FILE",
            help: "Output binary path"
        });
    }
    command.option({
        name: "cwd",
        short: "w",
        value_name: "DIR",
        help: "Working directory for intermediates"
    });
    command.flag({name: "debug", short: "d", help: "Enable debug compiler output"});
    command.flag({name: "release", short: "r", help: "Build with release optimizations"});
    command.flag({name: "strip", help: "Strip debug info from the final binary"});
    command.flag({name: "verbose", short: "v", help: "Enable verbose lifetime output"});
    command.positional({name: "package", help: "Package directory", required: false});
    return command;
}

func build_command() args.Command {
    return package_command("build", "Build the current package", true);
}

func install_command() args.Command {
    return package_command("install", "Build and install the current package", false);
}

func resolve_package_root(match: &args.Matches) string {
    let start = resolve_search_path(match.positional_at(0) ?? ".");
    if let package_root = find_package_root(start) {
        return package_root;
    }
    cli_error("package.jsonc not found in current directory or any parent");
}

func canonical_package_root(root: string) string {
    if filepath.base(root) == "." {
        return filepath.dir(root);
    }
    return root;
}

func package_output_name(root: string) string {
    let name = filepath.base(root);
    if name == "." {
        return filepath.base(filepath.dir(root));
    }
    return name;
}

func resolve_working_dir(match: &args.Matches, package_root: string) string {
    return match.option("cwd") ?? filepath.join(package_root, "build");
}

struct InstallConfig {
    name: ?string = null;
    include: Array<string> = [];
}

func parse_install_config(package_root: string) InstallConfig {
    let config_path = filepath.join(package_root, "package.jsonc");
    let config_text = try fs.read_file(config_path) catch fs.FileError as err {
        cli_error(err.message());
    };
    return try json.parse<InstallConfig>(config_text) catch json.ParseError as err {
        cli_error(err.message());
    };
}

func resolve_source_install_dir(package_name: string) string {
    let chi_src = os.env("CHI_SRC") ?? "";
    if !chi_src.is_empty() {
        return resolve_search_path(chi_src);
    }

    let chi_root = os.env("CHI_ROOT") ?? "";
    if !chi_root.is_empty() {
        return filepath.join(resolve_search_path(chi_root), "src", package_name);
    }

    cli_error("CHI_SRC is not set and CHI_ROOT is not set");
}

func normalize_package_path(path: string) string {
    return path.replace_all("\\", "/");
}

func is_safe_package_relative_path(path: string) bool {
    if path.is_empty() || filepath.is_absolute(path) {
        return false;
    }

    let normalized = normalize_package_path(path);
    if normalized == ".." || normalized.starts_with("../") || normalized.ends_with("/..") ||
       normalized.contains("/../") {
        return false;
    }
    return true;
}

func is_default_source_file(path: string) bool {
    return path.ends_with(".xs") || path.ends_with(".x") || path.ends_with(".c") ||
           path.ends_with(".h");
}

func copy_source_file(src: string, dest: string) {
    fs.mkdir_all(filepath.dir(dest));
    fs.copy_file(src, dest);
}

func copy_filtered_source_tree(src_root: string, dest_root: string, relative: string = "") {
    let current_src = filepath.join(src_root, relative);
    let entries = fs.list_dir(current_src);
    for entry in entries {
        let child_relative = filepath.join(relative, entry);
        let child_src = filepath.join(src_root, child_relative);
        let child_dest = filepath.join(dest_root, child_relative);
        if fs.is_dir(child_src) {
            copy_filtered_source_tree(src_root, dest_root, child_relative);
        } else if child_relative == "package.jsonc" || is_default_source_file(child_relative) {
            copy_source_file(child_src, child_dest);
        }
    }
}

func copy_included_path(root: string, dest: string, include_path: string) {
    let src_path = filepath.join(root, include_path);
    let dest_path = filepath.join(dest, include_path);
    if fs.exists(src_path) {
        if fs.is_dir(src_path) {
            fs.copy_all(src_path, dest_path);
            return;
        }

        copy_source_file(src_path, dest_path);
        return;
    }

    let matches = try fs.glob(include_path, root) catch fs.FileError as err {
        cli_error(err.message());
    };
    if matches.length == 0 {
        return;
    }
    for match in matches {
        copy_source_file(filepath.join(root, match), filepath.join(dest, match));
    }
}

func copy_package_source(root: string, parent: string, dest: string, include_paths: Array<string>) {
    fs.remove_all(dest);
    fs.mkdir_all(parent);
    copy_filtered_source_tree(root, dest);
    for include_path in include_paths {
        copy_included_path(root, dest, include_path);
    }
}

func install_package_source(
    package_root: string,
    working_dir: string,
    package_name: string,
    include_paths: Array<string>
) {
    let source_dir = resolve_source_install_dir(package_name);
    let root = canonical_package_root(package_root);
    let dest = source_dir;
    let parent = filepath.dir(dest);
    for include_path in include_paths {
        if !is_safe_package_relative_path(include_path) {
            cli_error("invalid package include path: " + include_path);
        }
    }

    try copy_package_source(root, parent, dest, include_paths) catch fs.FileError as err {
        cli_error(err.message());
    };

    let normalized_working_dir = canonical_package_root(working_dir);
    if normalized_working_dir.starts_with(root + filepath.separator_string()) {
        let relative = normalized_working_dir.byte_slice(root.byte_length() + 1, normalized_working_dir.byte_length());
        let nested = filepath.join(dest, relative);
        try fs.remove_all(nested) catch fs.FileError as err {
            cli_error(err.message());
        };
    }

    println("installed source " + dest);
}

func install_named_source(match: &args.Matches, package_root: string) {
    let install_config = parse_install_config(package_root);
    let package_name = install_config.name ?? "";
    if package_name.is_empty() {
        return;
    }

    let working_dir = resolve_working_dir(match, package_root);
    install_package_source(package_root, working_dir, package_name, install_config.include);
}

func run_chi_compiler(match: &args.Matches, package_root: string, output: string, default_release: bool = false) int32 {
    let chic = find_chic();
    let working_dir = resolve_working_dir(match, package_root);

    var cmd = [chic, "-p", package_root, "-o", output, "-w", working_dir];
    let debug = match.flag("debug");
    let release = match.flag("release");
    if debug {
        cmd.push("-d");
    }
    if release || (default_release && !debug) {
        cmd.push("-r");
    }
    if match.flag("strip") {
        cmd.push("--strip");
    }
    if match.flag("verbose") {
        cmd.push("-v");
    }
    return os.command(cmd.span());
}

func resolve_install_dir() string {
    let chi_path = os.env("CHI_PATH") ?? "";
    if !chi_path.is_empty() {
        return resolve_search_path(chi_path);
    }

    let chi_root = os.env("CHI_ROOT") ?? "";
    if !chi_root.is_empty() {
        return filepath.join(resolve_search_path(chi_root), "bin");
    }

    cli_error("CHI_PATH is not set and CHI_ROOT is not set");
}

func run_build_inner(match: &args.Matches) int32 {
    let package_root = canonical_package_root(resolve_package_root(match));
    let output = match.option("output") ?? filepath.join(package_root, package_output_name(package_root));
    return run_chi_compiler(match, package_root, output);
}

func run_build(match: &args.Matches) int32 {
    return try run_build_inner(match) catch CliError as err {
        println(err.message());
        1
    };
}

func run_install_inner(match: &args.Matches) int32 {
    let package_root = canonical_package_root(resolve_package_root(match));
    let bin_dir = resolve_install_dir();
    try fs.mkdir_all(bin_dir) catch fs.FileError as err {
        cli_error(err.message());
    };

    let output = filepath.join(bin_dir, package_output_name(package_root));
    let status = run_chi_compiler(match, package_root, output, true);
    if status != 0 {
        return status;
    }
    install_named_source(match, package_root);
    println("installed " + output);
    return 0;
}

func run_install(match: &args.Matches) int32 {
    return try run_install_inner(match) catch CliError as err {
        println(err.message());
        1
    };
}

func root_command() args.Command {
    var root = args.Command{
        name: "chi",
        summary: "Chi toolchain"
    };
    root.command(build_command());
    root.command(install_command());
    return root;
}

func main() {
    let cli = root_command();
    let parsed = cli.parse();
    switch parsed {
        args.Parse.Ok(match) => {
            let sub = match.subcommand();
            if sub != null {
                let command = sub!;
                if command.command_name() == "build" {
                    os.exit(run_build(command));
                }
                if command.command_name() == "install" {
                    os.exit(run_install(command));
                }
            }
            println(cli.help());
            return;
        },
        args.Parse.Help(text) => {
            println(text);
            return;
        },
        args.Parse.Error(text) => {
            println(text);
            os.exit(1);
        }
    }
}
