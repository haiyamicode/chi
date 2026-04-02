extern "C" {
    unsafe func __cx_argc() int32;
    unsafe func __cx_argv(index: int32) *byte;
    unsafe func __cx_strlen(s: *byte) uint32;
}

export func argv() Array<string> {
    var result: Array<string> = [];
    unsafe {
        let argc = __cx_argc();
        for i in 0..argc {
            let raw = __cx_argv(i);
            if raw {
                result.push(string.from_raw(raw, __cx_strlen(raw)));
            } else {
                result.push("");
            }
        }
    }
    return result;
}

export struct Flag {
    name: string = "";
    short: string = "";
    help: string = "";
}

export struct Option {
    name: string = "";
    short: string = "";
    value_name: string = "VALUE";
    help: string = "";
    multiple: bool = false;
}

export struct Positional {
    name: string = "";
    help: string = "";
    required: bool = true;
}

struct MatchValue {
    name: string = "";
    value: string = "";
}

const ERROR_DUPLICATE_SCHEMA = "duplicate {} '{}'";
const ERROR_DUPLICATE_SCHEMA_IN_COMMAND = "duplicate {} '{}' in command '{}'";
const ERROR_DUPLICATE_FLAG = "duplicate flag '--{}'";
const ERROR_DUPLICATE_OPTION = "duplicate option '--{}'";
const ERROR_REQUIRED_POSITIONAL_AFTER_OPTIONAL = "required positional '{}' cannot follow optional positional in command '{}'";


export struct Matches {
    command: string = "";
    flags: Array<string> = [];
    options: Array<MatchValue> = [];
    positionals: Array<string> = [];
    child_matches: Array<Matches> = [];

    func command_name() string {
        return this.command;
    }

    func subcommand() ?(&'this Matches) {
        if this.child_matches.length == 0 {
            return null;
        }
        return &this.child_matches[0];
    }

    func flag(name: string) bool {
        for flag in this.flags {
            if flag == name {
                return true;
            }
        }
        return false;
    }

    func option(name: string) ?string {
        for opt in this.options {
            if opt.name == name {
                return opt.value;
            }
        }
        return null;
    }

    func option_all(name: string) Array<string> {
        var result: Array<string> = [];
        for opt in this.options {
            if opt.name == name {
                result.push(opt.value);
            }
        }
        return result;
    }

    func positional_at(index: uint32) ?string {
        if index >= this.positionals.length {
            return null;
        }
        return this.positionals[index];
    }
}

export enum Parse {
    Ok(Matches),
    Help(string),
    Error(string)
}

enum ParseStep {
    Ok,
    Help(string),
    Error(string)
}

export struct Command {
    name: string = "";
    summary: string = "";

    flags: Array<Flag> = [];
    options: Array<Option> = [];
    positionals: Array<Positional> = [];
    commands: Array<Command> = [];
    private long_names: Map<string, bool> = {};
    private short_names: Map<string, bool> = {};
    private command_names: Map<string, bool> = {};
    private optional_positionals_started: bool = false;

    mutex func flag(spec: Flag) {
        if this.long_names.get(spec.name) {
            panic(duplicate_schema_text("flag", stringf("--{}", spec.name), this.name));
        }
        if !spec.short.is_empty() {
            if this.short_names.get(spec.short) {
                panic(duplicate_schema_text("flag", stringf("-{}", spec.short), this.name));
            }
            this.short_names.set(spec.short, true);
        }
        this.long_names.set(spec.name, true);
        this.flags.push(move spec);
    }

    mutex func option(spec: Option) {
        if this.long_names.get(spec.name) {
            panic(duplicate_schema_text("option", stringf("--{}", spec.name), this.name));
        }
        if !spec.short.is_empty() {
            if this.short_names.get(spec.short) {
                panic(duplicate_schema_text("option", stringf("-{}", spec.short), this.name));
            }
            this.short_names.set(spec.short, true);
        }
        this.long_names.set(spec.name, true);
        this.options.push(move spec);
    }

    mutex func positional(spec: Positional) {
        if spec.required {
            if this.optional_positionals_started {
                panic(stringf(ERROR_REQUIRED_POSITIONAL_AFTER_OPTIONAL, spec.name, this.name));
            }
        } else {
            this.optional_positionals_started = true;
        }
        this.positionals.push(move spec);
    }

    mutex func command(spec: Command) {
        if this.command_names.get(spec.name) {
            panic(duplicate_schema_text("command", spec.name, this.name));
        }
        this.command_names.set(spec.name, true);
        this.commands.push(move spec);
    }

    func parse() Parse {
        let argv_values = argv();
        if argv_values.length > 0 {
            return this.parse_from(argv_values.span(1));
        }
        return this.parse_from(argv_values.span(0, 0));
    }

    func parse_from(argv_values: &[string]) Parse {
        var matches = Matches{};
        matches.command = this.name;
        var parser = CommandParser{
            matches: &mutex matches,
            cmd: &this,
            :argv_values,
            command_name: this.name
        };
        let step = parser.parse();
        switch step {
            Ok => {},
            Help(text) => {
                return Help{text};
            },
            Error(text) => {
                return Error{text};
            }
        }
        return Ok{move matches};
    }

    func usage() string {
        return usage_for(&this, this.name);
    }

    func help() string {
        return help_for(&this, this.name);
    }
}

func append_help_lines(buf: &mutex Buffer, title: string, lines: &[string]) {
    if lines.length == 0 {
        return;
    }
    buf.write_string(title);
    buf.write_string(":\n");
    for line in lines {
        buf.write_string("  ");
        buf.write_string(line);
        buf.write_string("\n");
    }
}

enum ScanState {
    Options,
    PositionalOnly
}

enum ArgTokenKind {
    StopOptions,
    LongName,
    LongNameValue,
    ShortCluster,
    Value
}

struct ArgToken {
    kind: ArgTokenKind = ArgTokenKind.Value;
    first: string = "";
    second: string = "";
}

enum TokenStepKind {
    Next,
    Done,
    Help,
    Error
}

struct TokenStep {
    kind: TokenStepKind = TokenStepKind.Next;
    next_index: uint32 = 0;
    text: string = "";
}

func find_byte(text: string, ch: byte) ?uint32 {
    for i in 0..text.byte_length() {
        if text.byte_at(i) == ch {
            return i;
        }
    }
    return null;
}

func command_path(parent: string, name: string) string {
    if parent.is_empty() {
        return name;
    }
    if name.is_empty() {
        return parent;
    }
    return parent + " " + name;
}

func find_command_index(cmd: &Command, name: string) ?uint32 {
    for i in 0..cmd.commands.length {
        let child = &cmd.commands[i];
        if child.name == name {
            return i;
        }
    }
    return null;
}

func find_flag_long_index(cmd: &Command, name: string) ?uint32 {
    for i in 0..cmd.flags.length {
        let flag = &cmd.flags[i];
        if flag.name == name {
            return i;
        }
    }
    return null;
}

func find_flag_short_index(cmd: &Command, short: string) ?uint32 {
    for i in 0..cmd.flags.length {
        let flag = &cmd.flags[i];
        if flag.short == short {
            return i;
        }
    }
    return null;
}

func find_option_long_index(cmd: &Command, name: string) ?uint32 {
    for i in 0..cmd.options.length {
        let option = &cmd.options[i];
        if option.name == name {
            return i;
        }
    }
    return null;
}

func find_option_short_index(cmd: &Command, short: string) ?uint32 {
    for i in 0..cmd.options.length {
        let option = &cmd.options[i];
        if option.short == short {
            return i;
        }
    }
    return null;
}

func duplicate_schema_text(kind: string, name: string, command_name: string) string {
    if command_name.is_empty() {
        return stringf(ERROR_DUPLICATE_SCHEMA, kind, name);
    }
    return stringf(ERROR_DUPLICATE_SCHEMA_IN_COMMAND, kind, name, command_name);
}

func usage_for(cmd: &Command, name: string) string {
    var buf = Buffer{};
    buf.write_string("usage:");
    if !name.is_empty() {
        buf.write_string(" ");
        buf.write_string(name);
    }
    if cmd.commands.length > 0 {
        buf.write_string(" <command>");
    }
    if cmd.flags.length > 0 || cmd.options.length > 0 {
        buf.write_string(" [options]");
    }
    for positional in cmd.positionals {
        if positional.required {
            buf.write_string(" <");
        } else {
            buf.write_string(" [");
        }
        buf.write_string(positional.name);
        if positional.required {
            buf.write_string(">");
        } else {
            buf.write_string("]");
        }
    }
    return buf.to_string();
}

func help_for(cmd: &Command, name: string) string {
    var buf = Buffer{};
    buf.write_string(usage_for(cmd, name));
    if !cmd.summary.is_empty() {
        buf.write_string("\n\n");
        buf.write_string(cmd.summary);
    }

    var command_lines: Array<string> = [];
    for command in cmd.commands {
        if command.summary.is_empty() {
            command_lines.push(command.name);
        } else {
            command_lines.push(stringf("{} - {}", command.name, command.summary));
        }
    }

    var flag_lines: Array<string> = [];
    flag_lines.push("-h, --help - Show help");
    for flag in cmd.flags {
        let names = flag.short.is_empty() ? stringf("--{}", flag.name) : stringf(
            "-{}, --{}",
            flag.short,
            flag.name
        );
        if flag.help.is_empty() {
            flag_lines.push(names);
        } else {
            flag_lines.push(stringf("{} - {}", names, flag.help));
        }
    }

    var option_lines: Array<string> = [];
    for option in cmd.options {
        let names = option.short.is_empty() ? stringf("--{} <{}>", option.name, option.value_name) : stringf(
            "-{} <{}>, --{} <{}>",
            option.short,
            option.value_name,
            option.name,
            option.value_name
        );
        if option.help.is_empty() {
            option_lines.push(names);
        } else {
            option_lines.push(stringf("{} - {}", names, option.help));
        }
    }

    var positional_lines: Array<string> = [];
    for positional in cmd.positionals {
        if positional.help.is_empty() {
            positional_lines.push(positional.name);
        } else {
            positional_lines.push(stringf("{} - {}", positional.name, positional.help));
        }
    }

    if command_lines.length > 0 || flag_lines.length > 0 || option_lines.length > 0 || positional_lines.length > 0 {
        buf.write_string("\n\n");
    }
    append_help_lines(&mutex buf, "commands", command_lines.span());
    append_help_lines(&mutex buf, "flags", flag_lines.span());
    append_help_lines(&mutex buf, "options", option_lines.span());
    append_help_lines(&mutex buf, "positionals", positional_lines.span());
    return buf.to_string().trim_right();
}

func parse_error_text<'a>(cmd: &'a Command, name: string, message: string) string {
    return message + "\n\n" + usage_for(cmd, name);
}

func scan_arg(arg: string, state: ScanState) ArgToken {
    if state == ScanState.PositionalOnly {
        return {kind: ArgTokenKind.Value, first: arg};
    }
    if arg == "--" {
        return {kind: ArgTokenKind.StopOptions};
    }
    if arg.starts_with("--") && arg.byte_length() > 2 {
        let body = arg.byte_slice(2, arg.byte_length());
        if let eq = find_byte(body, '=') {
            return {
                kind: ArgTokenKind.LongNameValue,
                first: body.byte_slice(0, eq),
                second: body.byte_slice(eq + 1, body.byte_length())
            };
        }
        return {kind: ArgTokenKind.LongName, first: body};
    }
    if arg.starts_with("-") && arg.byte_length() > 1 {
        return {kind: ArgTokenKind.ShortCluster, first: arg.byte_slice(1, arg.byte_length())};
    }
    return {kind: ArgTokenKind.Value, first: arg};
}

struct CommandParser {
    matches: &mutex Matches;
    cmd: &Command;
    argv_values: &[string];
    command_name: string = "";
    positional_index: uint32 = 0;
    state: ScanState = ScanState.Options;
    index: uint32 = 0;

    mutex func add_flag(flag: &Flag) ?string {
        if this.matches.flag(flag.name) {
            return stringf(ERROR_DUPLICATE_FLAG, flag.name);
        }
        this.matches.flags.push(flag.name);
        return null;
    }

    mutex func add_option(option: &Option, value: string) ?string {
        if !option.multiple && this.matches.option(option.name) {
            return stringf(ERROR_DUPLICATE_OPTION, option.name);
        }
        this.matches.options.push({name: option.name, :value});
        return null;
    }

    func at_first_positional() bool {
        return this.positional_index < 1;
    }

    mutex func parse_subcommand(subcommand: &Command) ParseStep {
        var child_matches = Matches{};
        child_matches.command = subcommand.name;
        var child_parser = CommandParser{
            matches: &mutex child_matches,
            cmd: subcommand,
            argv_values: this.argv_values,
            command_name: command_path(this.command_name, subcommand.name),
            index: this.index + 1
        };
        let child_step = child_parser.parse();
        switch child_step {
            Ok => {},
            Help(text) => {
                return Help{text};
            },
            Error(text) => {
                return Error{text};
            }
        }
        this.matches.child_matches.push(move child_matches);
        return ParseStep.Ok;
    }

    mutex func parse_long_name(name: string) TokenStep {
        if name == "help" {
            return {kind: TokenStepKind.Help, text: help_for(this.cmd, this.command_name)};
        }
        if let flag_index = find_flag_long_index(this.cmd, name) {
            if let message = this.add_flag(&this.cmd.flags[flag_index]) {
                return {
                    kind: TokenStepKind.Error,
                    text: parse_error_text(this.cmd, this.command_name, message)
                };
            }
            return {kind: TokenStepKind.Next, next_index: this.index + 1};
        }
        if let option_index = find_option_long_index(this.cmd, name) {
            let option = &this.cmd.options[option_index];
            if this.index + 1 >= this.argv_values.length {
                return {
                    kind: TokenStepKind.Error,
                    text: parse_error_text(
                        this.cmd,
                        this.command_name,
                        stringf("missing value for option '--{}'", option.name)
                    )
                };
            }
            if let message = this.add_option(option, this.argv_values[this.index + 1]) {
                return {
                    kind: TokenStepKind.Error,
                    text: parse_error_text(this.cmd, this.command_name, message)
                };
            }
            return {kind: TokenStepKind.Next, next_index: this.index + 2};
        }
        return {
            kind: TokenStepKind.Error,
            text: parse_error_text(
                this.cmd,
                this.command_name,
                stringf("unknown option '--{}'", name)
            )
        };
    }

    mutex func parse_long_name_value(name: string, value: string) TokenStep {
        if name == "help" {
            return {kind: TokenStepKind.Help, text: help_for(this.cmd, this.command_name)};
        }
        if let flag_index = find_flag_long_index(this.cmd, name) {
            let flag = &this.cmd.flags[flag_index];
            return {
                kind: TokenStepKind.Error,
                text: parse_error_text(
                    this.cmd,
                    this.command_name,
                    stringf("flag '--{}' does not take a value", flag.name)
                )
            };
        }
        if let option_index = find_option_long_index(this.cmd, name) {
            if let message = this.add_option(&this.cmd.options[option_index], value) {
                return {
                    kind: TokenStepKind.Error,
                    text: parse_error_text(this.cmd, this.command_name, message)
                };
            }
            return {kind: TokenStepKind.Next, next_index: this.index + 1};
        }
        return {
            kind: TokenStepKind.Error,
            text: parse_error_text(
                this.cmd,
                this.command_name,
                stringf("unknown option '--{}'", name)
            )
        };
    }

    mutex func parse_short_cluster(cluster: string) TokenStep {
        var short_index: uint32 = 0;
        while short_index < cluster.byte_length() {
            let short_name = cluster.byte_slice(short_index, short_index + 1);
            if short_name == "h" {
                return {kind: TokenStepKind.Help, text: help_for(this.cmd, this.command_name)};
            }

            if let flag_index = find_flag_short_index(this.cmd, short_name) {
                if let message = this.add_flag(&this.cmd.flags[flag_index]) {
                    return {
                        kind: TokenStepKind.Error,
                        text: parse_error_text(this.cmd, this.command_name, message)
                    };
                }
                short_index += 1;
                continue;
            }

            if let option_index = find_option_short_index(this.cmd, short_name) {
                let option = &this.cmd.options[option_index];
                if short_index + 1 < cluster.byte_length() {
                    if let message = this.add_option(
                        option,
                        cluster.byte_slice(short_index + 1, cluster.byte_length())
                    ) {
                        return {
                            kind: TokenStepKind.Error,
                            text: parse_error_text(this.cmd, this.command_name, message)
                        };
                    }
                    return {kind: TokenStepKind.Next, next_index: this.index + 1};
                }
                if this.index + 1 >= this.argv_values.length {
                    return {
                        kind: TokenStepKind.Error,
                        text: parse_error_text(
                            this.cmd,
                            this.command_name,
                            stringf("missing value for option '-{}'", option.short)
                        )
                    };
                }
                if let message = this.add_option(option, this.argv_values[this.index + 1]) {
                    return {
                        kind: TokenStepKind.Error,
                        text: parse_error_text(this.cmd, this.command_name, message)
                    };
                }
                return {kind: TokenStepKind.Next, next_index: this.index + 2};
            }

            return {
                kind: TokenStepKind.Error,
                text: parse_error_text(
                    this.cmd,
                    this.command_name,
                    stringf("unknown option '-{}'", short_name)
                )
            };
        }

        return {kind: TokenStepKind.Next, next_index: this.index + 1};
    }

    mutex func parse_value_token(value: string) TokenStep {
        if this.at_first_positional() {
            if let child_index = find_command_index(this.cmd, value) {
                let subcommand = &this.cmd.commands[child_index];
                let child_step = this.parse_subcommand(subcommand);
                switch child_step {
                    Ok => {},
                    Help(text) => {
                        return {kind: TokenStepKind.Help, :text};
                    },
                    Error(text) => {
                        return {kind: TokenStepKind.Error, :text};
                    }
                }
                return {kind: TokenStepKind.Done};
            }
        }

        if this.positional_index < this.cmd.positionals.length {
            this.matches.positionals.push(value);
            this.positional_index += 1;
            return {kind: TokenStepKind.Next, next_index: this.index + 1};
        }

        if this.cmd.commands.length > 0 && this.at_first_positional() {
            return {
                kind: TokenStepKind.Error,
                text: stringf(
                    "unknown command '{}'\n\n{}",
                    value,
                    help_for(this.cmd, this.command_name)
                )
            };
        }

        return {
            kind: TokenStepKind.Error,
            text: parse_error_text(
                this.cmd,
                this.command_name,
                stringf("unexpected argument '{}'", value)
            )
        };
    }

    mutex func parse() ParseStep {
        while this.index < this.argv_values.length {
            let token = scan_arg(this.argv_values[this.index], this.state);
            var step = TokenStep{};
            switch token.kind {
                StopOptions => {
                    this.state = ScanState.PositionalOnly;
                    this.index += 1;
                    continue;
                },
                LongName => {
                    step = this.parse_long_name(token.first);
                },
                LongNameValue => {
                    step = this.parse_long_name_value(token.first, token.second);
                },
                ShortCluster => {
                    step = this.parse_short_cluster(token.first);
                },
                Value => {
                    step = this.parse_value_token(token.first);
                }
            }

            switch step.kind {
                Next => {
                    this.index = step.next_index;
                },
                Done => {
                    return ParseStep.Ok;
                },
                Help => {
                    return Help{step.text};
                },
                Error => {
                    return Error{step.text};
                }
            }
        }

        if this.positional_index < this.cmd.positionals.length {
            let positional = &this.cmd.positionals[this.positional_index];
            if positional.required {
                return Error{
                    parse_error_text(
                        this.cmd,
                        this.command_name,
                        stringf("missing positional '{}'", positional.name)
                    )
                };
            }
        }

        return ParseStep.Ok;
    }
}
