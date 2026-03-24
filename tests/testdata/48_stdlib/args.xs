import "std/args" as args;

func main() {
    let argv = args.argv();
    printf("argc = {}\n", argv.length);
    printf("has exe = {}\n", argv.length > 0);
    if argv.length > 0 {
        printf("exe contains test name = {}\n", argv[0].contains("48_stdlib_args"));
    }

    var cli = args.Command{name: "chi", summary: "Chi command line"};
    cli.flag({
        name: "verbose",
        short: "v",
        help: "Verbose output"
    });

    var build = args.Command{name: "build", summary: "Build a package"};
    build.option(
        {
            name: "output",
            short: "o",
            value_name: "FILE",
            help: "Output path"
        }
    );
    build.option(
        {
            name: "define",
            short: "D",
            value_name: "NAME",
            help: "Define a symbol",
            multiple: true
        }
    );
    build.flag({
        name: "release",
        short: "r",
        help: "Release mode"
    });
    build.positional({name: "input", help: "Input file"});
    cli.command(move build);

    var echo = args.Command{name: "echo", summary: "Echo args"};
    echo.positional({name: "message", help: "Message"});
    cli.command(move echo);

    var show = args.Command{name: "show", summary: "Show target"};
    show.positional({
        name: "target",
        help: "Target",
        required: false
    });
    cli.command(move show);

    println("=== parse() smoke ===");
    let parsed_cli = cli.parse();
    switch parsed_cli {
        args.Parse.Ok(match) => {
            printf("command = {}\n", match.command_name());
            printf("has subcommand = {}\n", match.subcommand() != null);
        },
        else => {
            println("unexpected non-ok result");
        }
    }

    println("=== parse build ===");
    let build_argv = ["build", "--output=out", "-r", "main.xs"];
    let parsed = cli.parse_from(build_argv.span());
    switch parsed {
        args.Parse.Ok(match) => {
            printf("root verbose = {}\n", match.flag("verbose"));
            let sub = match.subcommand()!;
            printf("command = {}\n", sub.command_name());
            printf("release = {}\n", sub.flag("release"));
            printf("output = {}\n", sub.option("output")!);
            printf("input = {}\n", sub.positional_at(0)!);
        },
        args.Parse.Help(text) => {
            printf("unexpected help: {}\n", text);
        },
        args.Parse.Error(text) => {
            printf("unexpected error: {}\n", text);
        }
    }

    println("=== parse root flag and repeated options ===");
    let complex_argv = ["-v", "build", "-D", "ONE", "--define=TWO", "-o", "bin/chi", "main.xs"];
    let complex = cli.parse_from(complex_argv.span());
    switch complex {
        args.Parse.Ok(match) => {
            let sub = match.subcommand()!;
            let defines = sub.option_all("define");
            printf("root verbose = {}\n", match.flag("verbose"));
            printf("command = {}\n", sub.command_name());
            printf("defines = [{}, {}]\n", defines[0], defines[1]);
            printf("output = {}\n", sub.option("output")!);
        },
        else => {
            println("unexpected non-ok result");
        }
    }

    println("=== parse attached short option value ===");
    let attached_argv = ["build", "-DTHREE", "main.xs"];
    let attached = cli.parse_from(attached_argv.span());
    switch attached {
        args.Parse.Ok(match) => {
            let sub = match.subcommand()!;
            let defines = sub.option_all("define");
            printf("define = {}\n", defines[0]);
        },
        else => {
            println("unexpected non-ok result");
        }
    }

    println("=== stop options ===");
    let stop_argv = ["echo", "--", "--not-an-option"];
    let stop_result = cli.parse_from(stop_argv.span());
    switch stop_result {
        args.Parse.Ok(match) => {
            let sub = match.subcommand()!;
            printf("command = {}\n", sub.command_name());
            printf("message = {}\n", sub.positional_at(0)!);
        },
        else => {
            println("unexpected non-ok result");
        }
    }

    println("=== optional positional omitted ===");
    let optional_positional_argv = ["show"];
    let optional_positional_result = cli.parse_from(optional_positional_argv.span());
    switch optional_positional_result {
        args.Parse.Ok(match) => {
            let sub = match.subcommand()!;
            printf("command = {}\n", sub.command_name());
            printf("target present = {}\n", sub.positional_at(0) != null);
        },
        else => {
            println("unexpected non-ok result");
        }
    }

    println("=== help ===");
    let help_argv = ["build", "--help"];
    let help_result = cli.parse_from(help_argv.span());
    switch help_result {
        args.Parse.Help(text) => {
            println(text);
        },
        else => {
            println("unexpected non-help result");
        }
    }

    println("=== root help ===");
    let root_help_argv = ["--help"];
    let root_help_result = cli.parse_from(root_help_argv.span());
    switch root_help_result {
        args.Parse.Help(text) => {
            println(text);
        },
        else => {
            println("unexpected non-help result");
        }
    }

    println("=== missing option value ===");
    let missing_value_argv = ["build", "-o"];
    let missing_value_result = cli.parse_from(missing_value_argv.span());
    switch missing_value_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== unknown long option ===");
    let unknown_long_argv = ["build", "--wat", "main.xs"];
    let unknown_long_result = cli.parse_from(unknown_long_argv.span());
    switch unknown_long_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== unknown short option ===");
    let unknown_short_argv = ["build", "-x", "main.xs"];
    let unknown_short_result = cli.parse_from(unknown_short_argv.span());
    switch unknown_short_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== duplicate single option ===");
    let duplicate_option_argv = ["build", "-o", "one", "-o", "two", "main.xs"];
    let duplicate_option_result = cli.parse_from(duplicate_option_argv.span());
    switch duplicate_option_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== duplicate flag ===");
    let duplicate_flag_argv = ["-v", "-v"];
    let duplicate_flag_result = cli.parse_from(duplicate_flag_argv.span());
    switch duplicate_flag_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== missing positional ===");
    let missing_positional_argv = ["build", "-r"];
    let missing_positional_result = cli.parse_from(missing_positional_argv.span());
    switch missing_positional_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== unexpected positional ===");
    let unexpected_positional_argv = ["echo", "one", "two"];
    let unexpected_positional_result = cli.parse_from(unexpected_positional_argv.span());
    switch unexpected_positional_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }

    println("=== unknown command ===");
    let unknown_command_argv = ["ship"];
    let unknown_command_result = cli.parse_from(unknown_command_argv.span());
    switch unknown_command_result {
        args.Parse.Error(text) => {
            println(text);
        },
        else => {
            println("unexpected non-error result");
        }
    }
}
