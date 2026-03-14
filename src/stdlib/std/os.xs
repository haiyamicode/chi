// std/os — operating system utilities

extern "C" {
    unsafe func __cx_getenv(key: *byte) *byte;
    unsafe func __cx_setenv(key: *byte, value: *byte);
    unsafe func __cx_exit(code: int);
    unsafe func __cx_getcwd() *byte;
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

