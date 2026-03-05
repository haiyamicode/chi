// std/conv — string to value conversions

extern "C" {
    unsafe func __cx_parse_int(str: *byte, out: *int64) int;
    unsafe func __cx_parse_float(str: *byte, out: *float64) int;
}

export func parse_int(s: string) ?int {
    var result: int64 = 0;
    var cs = s.to_cstring();
    unsafe {
        var ok = __cx_parse_int(cs.as_ptr(), &result as *int64);
        if ok == 0 {
            return null;
        }
    }
    return result as int;
}

export func parse_float(s: string) ?float64 {
    var result: float64 = 0.0;
    var cs = s.to_cstring();
    unsafe {
        var ok = __cx_parse_float(cs.as_ptr(), &result as *float64);
        if ok == 0 {
            return null;
        }
    }
    return result;
}

export func parse_bool(s: string) ?bool {
    if s == "true" || s == "1" {
        return true;
    }
    if s == "false" || s == "0" {
        return false;
    }
    return null;
}
