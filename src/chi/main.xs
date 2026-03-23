import "std/args" as args;
import "std/fs" as fs;
import "std/os" as os;
import "std/path" as path;

func find_chic() string {
    let argv_values = args.argv();
    if argv_values.length > 0 {
        return path.join(path.dir(argv_values[0]), "chic");
    }
    return "chic";
}

func resolve_search_path(input: string) string {
    if path.is_absolute(input) {
        return input;
    }
    return path.join(os.cwd(), input);
}

func find_package_root(start: string) ?string {
    var current = start;
    while true {
        let config_path = path.join(current, "package.jsonc");
        if fs.exists(config_path) {
            return current;
        }
        let parent = path.dir(current);
        if parent == current {
            return null;
        }
        current = parent;
    }
}

func build_command() args.Command {
    var build = args.Command{
        name: "build",
        summary: "Build the current package"
    };
    build.option({
        name: "output",
        short: "o",
        value_name: "FILE",
        help: "Output binary path"
    });
    build.option({
        name: "cwd",
        short: "w",
        value_name: "DIR",
        help: "Working directory for intermediates"
    });
    build.flag({name: "debug", short: "d", help: "Enable debug compiler output"});
    build.flag({name: "verbose", short: "v", help: "Enable verbose lifetime output"});
    build.positional({name: "package", help: "Package directory", required: false});
    return build;
}

func run_build(match: &args.Matches) int32 {
    let chic = find_chic();
    let start = resolve_search_path(match.positional_at(0) ?? ".");
    let package_root = find_package_root(start);
    if package_root == null {
        println("package.jsonc not found in current directory or any parent");
        return 1;
    }
    let root = package_root!;
    let output = match.option("output") ?? path.join(root, path.base(root));
    let working_dir = match.option("cwd") ?? path.join(root, "build");

    var cmd = [chic, "-p", root, "-o", output, "-w", working_dir];
    if match.flag("debug") {
        cmd.push("-d");
    }
    if match.flag("verbose") {
        cmd.push("-v");
    }
    return os.command(cmd.span());
}

func root_command() args.Command {
    var root = args.Command{
        name: "chi",
        summary: "Chi toolchain"
    };
    root.command(build_command());
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
