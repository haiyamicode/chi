// std/os — operating system utilities

export struct CommandResult {
    exit_code: int32 = -1;
    stdout: string = "";
    stderr: string = "";

    func is_ok() bool {
        return this.exit_code == 0;
    }
}

extern "C" {
    unsafe func __cx_getenv(key: *byte) *byte;
    unsafe func __cx_setenv(key: *byte, value: *byte);
    unsafe func __cx_exit(code: int);
    unsafe func __cx_getcwd() *byte;
    unsafe func __cx_system(command: *byte, result: *CommandResult);
    unsafe func __cx_command(args: *void, result: *CommandResult);
    unsafe func __cx_platform_tags(result: *void);
    unsafe func __cx_strlen(s: *byte) uint32;
}

export func env(key: string) ?string {
    var cs = key.to_cstring();
    unsafe {
        var result = __cx_getenv(cs.as_ptr());
        if result == null {
            return null;
        }
        return string.from_raw(result, __cx_strlen(result));
    }
}

export func set_env(key: string, value: string) {
    var k = key.to_cstring();
    var v = value.to_cstring();
    unsafe {
        __cx_setenv(k.as_ptr(), v.as_ptr());
    }
}

export func exit(code: int) never {
    unsafe {
        __cx_exit(code);
    }
}

export func cwd() string {
    unsafe {
        var result = __cx_getcwd();
        return string.from_raw(result, __cx_strlen(result));
    }
}

export func system(command: string) CommandResult {
    var cs = command.to_cstring();
    var result = CommandResult{};
    unsafe {
        __cx_system(cs.as_ptr(), &result);
    }
    return result;
}

export func command(args: &[string]) CommandResult {
    var result = CommandResult{};
    unsafe {
        __cx_command(&args, &result);
    }
    return result;
}

export func platform_tags() Array<string> {
    var result: Array<string> = [];
    unsafe {
        __cx_platform_tags(&result);
    }
    return result;
}
