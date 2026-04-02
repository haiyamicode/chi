import "std/args" as args;
import "std/filepath" as filepath;
import "std/fs" as fs;
import "std/json" as json;
import "std/os" as os;

extern "C" {
    unsafe func __cx_default_chi_home() *byte;
    unsafe func __cx_strlen(s: *byte) uint32;
}

func default_chi_home() ?string {
    unsafe {
        var result = __cx_default_chi_home();
        if result == null {
            return null;
        }
        return string.from_raw(result, __cx_strlen(result));
    }
}

struct CliError {
    detail: string = "";

    impl Error {
        func message() string {
            return this.detail;
        }
    }
}

struct InstallConfig {
    name: ?string = null;
    include: Array<string> = [];
}

struct CliApp {
    cwd: string = "";
    path_env: ?string = null;
    chi_path_dir: ?string = null;
    chi_root_dir: ?string = null;
    chi_src_dir: ?string = null;

    mut func new() {
        this.cwd = os.cwd();
        this.path_env = os.env("PATH");
        this.chi_path_dir = this.resolve_env_path("CHI_PATH");
        this.chi_root_dir = this.resolve_env_path("CHI_ROOT")
            ?? this.resolve_env_path("CHI_HOME")
            ?? default_chi_home();
        this.chi_src_dir = this.resolve_env_path("CHI_SRC");
    }

    func fail(message: string) never {
        throw new CliError{detail: move message};
    }

    func log_verbose(match: &args.Matches, message: string) {
        if match.flag("verbose") {
            println(message);
        }
    }

    func format_command(cmd: &[string]) string {
        var result = "";
        for part, i in cmd {
            if i > 0 {
                result += " ";
            }
            result += part;
        }
        return result;
    }

    func print_command_output(result: &os.CommandResult) {
        if !result.stdout.is_empty() {
            printf("{}", result.stdout);
        }
        if !result.stderr.is_empty() {
            printf("{}", result.stderr);
        }
    }

    func run_command(match: &args.Matches, cmd: &[string]) int32 {
        if match.flag("verbose") {
            println(this.format_command(cmd));
        }
        let result = os.command(cmd);
        if match.flag("verbose") || result.exit_code != 0 {
            this.print_command_output(&result);
        }
        return result.exit_code;
    }

    func resolve_search_path(input: string) string {
        if filepath.is_absolute(input) {
            return input;
        }
        return filepath.join(this.cwd, input);
    }

    func resolve_env_path(name: string) ?string {
        if let value = os.env(name) {
            return this.resolve_search_path(value);
        }
        return null;
    }

    func find_in_dir(dir: string, name: string) ?string {
        if dir.is_empty() {
            return null;
        }
        let candidate = filepath.join(dir, name);
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
            if let candidate = this.find_in_dir(entry, name) {
                return candidate;
            }
        }
        return null;
    }

    func require_chi_compiler() string {
        if let chi_path_dir = this.chi_path_dir {
            if let candidate = this.find_in_dir(chi_path_dir, "chic") {
                return candidate;
            }
        }

        if let chi_root_dir = this.chi_root_dir {
            if let candidate = this.find_in_dir(filepath.join(chi_root_dir, "bin"), "chic") {
                return candidate;
            }
        }

        if let path_value = this.path_env {
            if let candidate = this.find_in_path(path_value, "chic") {
                return candidate;
            }
        }

        this.fail("could not find Chi compiler binary (chic)");
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
        command.flag({name: "verbose", short: "v", help: "Print resolved compiler and output paths"});
        command.flag({name: "verbose-lifetimes", help: "Enable verbose lifetime analysis output"});
        command.flag({name: "verbose-generics", help: "Enable verbose generics output"});
        command.positional({name: "package", help: "Package directory", required: false});
        return command;
    }

    func build_command() args.Command {
        return this.package_command("build", "Build the current package", true);
    }

    func install_command() args.Command {
        return this.package_command("install", "Build and install the current package", false);
    }

    func require_package_root(match: &args.Matches) string {
        let start = this.resolve_search_path(match.positional_at(0) ?? ".");
        return this.find_package_root(start) ??
               this.fail("package.jsonc not found in current directory or any parent");
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

    func parse_install_config(package_root: string) InstallConfig {
        let config_path = filepath.join(package_root, "package.jsonc");
        let config_text = try fs.read_file(config_path) catch fs.FileError as err {
            this.fail(err.message());
        };
        return try json.parse<InstallConfig>(config_text) catch json.ParseError as err {
            this.fail(err.message());
        };
    }

    func require_source_install_dir(package_name: string) string {
        if let chi_src_dir = this.chi_src_dir {
            return chi_src_dir;
        }
        let chi_root_dir = this.chi_root_dir ?? this.fail("CHI_SRC is not set and CHI_ROOT is not set");
        return filepath.join(chi_root_dir, "src", package_name);
    }

    func normalize_package_path(path: string) string {
        return path.replace_all("\\", "/");
    }

    func is_safe_package_relative_path(path: string) bool {
        if path.is_empty() || filepath.is_absolute(path) {
            return false;
        }

        let normalized = this.normalize_package_path(path);
        if normalized == ".." || normalized.starts_with("../") ||
           normalized.ends_with("/..") || normalized.contains("/../") {
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
                this.copy_filtered_source_tree(src_root, dest_root, child_relative);
            } else if child_relative == "package.jsonc" ||
                      this.is_default_source_file(child_relative) {
                this.copy_source_file(child_src, child_dest);
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

            this.copy_source_file(src_path, dest_path);
            return;
        }

        let matches = try fs.glob(include_path, root) catch fs.FileError as err {
            this.fail(err.message());
        };
        if matches.length == 0 {
            return;
        }
        for match in matches {
            this.copy_source_file(filepath.join(root, match), filepath.join(dest, match));
        }
    }

    func copy_package_source(
        root: string,
        parent: string,
        dest: string,
        include_paths: Array<string>
    ) {
        fs.remove_all(dest);
        fs.mkdir_all(parent);
        this.copy_filtered_source_tree(root, dest);
        for include_path in include_paths {
            this.copy_included_path(root, dest, include_path);
        }
    }

    func install_package_source(
        package_root: string,
        working_dir: string,
        package_name: string,
        include_paths: Array<string>
    ) {
        let source_dir = this.require_source_install_dir(package_name);
        let root = this.canonical_package_root(package_root);
        let dest = source_dir;
        let parent = filepath.dir(dest);
        for include_path in include_paths {
            if !this.is_safe_package_relative_path(include_path) {
                this.fail("invalid package include path: " + include_path);
            }
        }

        try this.copy_package_source(root, parent, dest, include_paths) catch fs.FileError as err {
            this.fail(err.message());
        };

        let normalized_working_dir = this.canonical_package_root(working_dir);
        if normalized_working_dir.starts_with(root + filepath.separator_string()) {
            let relative = normalized_working_dir.byte_slice(
                root.byte_length() + 1,
                normalized_working_dir.byte_length()
            );
            let nested = filepath.join(dest, relative);
            try fs.remove_all(nested) catch fs.FileError as err {
                this.fail(err.message());
            };
        }
    }

    func install_named_source(match: &args.Matches, package_root: string) {
        let install_config = this.parse_install_config(package_root);
        let package_name = install_config.name ?? "";
        if package_name.is_empty() {
            return;
        }

        let working_dir = this.resolve_working_dir(match, package_root);
        this.log_verbose(match, "source install dir " + this.require_source_install_dir(package_name));
        this.install_package_source(package_root, working_dir, package_name, install_config.include);
    }

    func run_chi_compiler(
        match: &args.Matches,
        package_root: string,
        output: string,
        default_release: bool = false
    ) int32 {
        let chic = this.require_chi_compiler();
        let working_dir = this.resolve_working_dir(match, package_root);

        this.log_verbose(match, "compiler " + chic);
        this.log_verbose(match, "package root " + package_root);
        this.log_verbose(match, "working dir " + working_dir);
        this.log_verbose(match, "output " + output);

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
        if match.flag("verbose-lifetimes") {
            cmd.push("--verbose-lifetimes");
        }
        if match.flag("verbose-generics") {
            cmd.push("--verbose-generics");
        }
        return this.run_command(match, cmd.span());
    }

    func require_install_dir() string {
        if let chi_path_dir = this.chi_path_dir {
            return chi_path_dir;
        }
        let chi_root_dir = this.chi_root_dir ?? this.fail("CHI_PATH is not set and CHI_ROOT is not set");
        return filepath.join(chi_root_dir, "bin");
    }

    func run_build_inner(match: &args.Matches) int32 {
        let package_root = this.canonical_package_root(this.require_package_root(match));
        let output = match.option("output") ??
                     filepath.join(package_root, this.package_output_name(package_root));
        return this.run_chi_compiler(match, package_root, output);
    }

    func run_build(match: &args.Matches) int32 {
        return try this.run_build_inner(match) catch CliError as err {
            println(err.message());
            1
        };
    }

    func run_install_inner(match: &args.Matches) int32 {
        let package_root = this.canonical_package_root(this.require_package_root(match));
        let bin_dir = this.require_install_dir();
        this.log_verbose(match, "install dir " + bin_dir);
        try fs.mkdir_all(bin_dir) catch fs.FileError as err {
            this.fail(err.message());
        };

        let output = filepath.join(bin_dir, this.package_output_name(package_root));
        let status = this.run_chi_compiler(match, package_root, output, true);
        if status != 0 {
            return status;
        }
        this.install_named_source(match, package_root);
        return 0;
    }

    func run_install(match: &args.Matches) int32 {
        return try this.run_install_inner(match) catch CliError as err {
            println(err.message());
            1
        };
    }

    func root_command() args.Command {
        var root = args.Command{
            name: "chi",
            summary: "Chi toolchain"
        };
        root.command(this.build_command());
        root.command(this.install_command());
        return root;
    }

    func run() {
        let cli = this.root_command();
        let parsed = cli.parse();
        switch parsed {
            args.Parse.Ok(match) => {
                let sub = match.subcommand();
                if sub != null {
                    let command = sub!;
                    if command.command_name() == "build" {
                        os.exit(this.run_build(command));
                    }
                    if command.command_name() == "install" {
                        os.exit(this.run_install(command));
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
}

func main() {
    var app = CliApp{};
    app.run();
}
