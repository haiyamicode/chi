// std/filepath — path manipulation utilities using the platform default separator

extern "C" {
    unsafe func __cx_path_separator() int32;
}

func filepath_separator() byte {
    unsafe {
        return __cx_path_separator() as byte;
    }
}

export let PATH_SEPARATOR: byte = filepath_separator();

export func separator_string() string {
    if PATH_SEPARATOR == '\\' {
        return "\\";
    }
    return "/";
}

func is_separator(c: byte) bool {
    return c == PATH_SEPARATOR;
}

export func dir(p: string) string {
    var len = p.byte_length();
    var i = len;
    while i > 0 {
        i -= 1;
        if is_separator(p.byte_at(i)) {
            if i == 0 {
                return separator_string();
            }
            return p.byte_slice(0, i);
        }
    }
    return ".";
}

export func base(p: string) string {
    if p.is_empty() {
        return ".";
    }
    var end = p.byte_length();
    while end > 0 && is_separator(p.byte_at(end - 1)) {
        end -= 1;
    }
    if end == 0 {
        return separator_string();
    }
    var i = end;
    while i > 0 {
        i -= 1;
        if is_separator(p.byte_at(i)) {
            return p.byte_slice(i + 1, end);
        }
    }
    return p.byte_slice(0, end);
}

export func ext(p: string) string {
    var i = p.byte_length();
    while i > 0 {
        i -= 1;
        let c = p.byte_at(i);
        if c == '.' {
            return p.byte_slice(i, p.byte_length());
        }
        if is_separator(c) {
            break;
        }
    }
    return "";
}

export func is_absolute(p: string) bool {
    return !p.is_empty() && is_separator(p.byte_at(0));
}

export func join(...parts: string) string {
    var result = "";
    let separator = separator_string();
    for part in parts {
        if part.is_empty() {
            continue;
        }
        if result.is_empty() {
            result = part;
            continue;
        }
        if is_separator(result.byte_at(result.byte_length() - 1)) {
            result = result + part;
        } else {
            result = result + separator + part;
        }
    }
    return result;
}
