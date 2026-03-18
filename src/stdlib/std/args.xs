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
