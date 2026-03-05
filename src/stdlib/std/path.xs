// std/path — path manipulation utilities

export func dir(p: string) string {
    var len = p.byte_length();
    var i = len;
    while i > 0 {
        i -= 1;
        if p.byte_at(i) == '/' {
            if i == 0 {
                return "/";
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
    while end > 0 && p.byte_at(end - 1) == '/' {
        end -= 1;
    }
    if end == 0 {
        return "/";
    }
    var i = end;
    while i > 0 {
        i -= 1;
        if p.byte_at(i) == '/' {
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
        if c == '/' {
            break;
        }
    }
    return "";
}

export func is_absolute(p: string) bool {
    return !p.is_empty() && p.byte_at(0) == '/';
}

export func join(a: string, b: string) string {
    if a.is_empty() {
        return b;
    }
    if b.is_empty() {
        return a;
    }
    if a.byte_at(a.byte_length() - 1) == '/' {
        return a + b;
    }
    return a + "/" + b;
}

