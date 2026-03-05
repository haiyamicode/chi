// std/math — mathematical constants and functions (float64)

extern "C" {
    func __cx_sqrt(x: float64) float64;
    func __cx_pow(base: float64, exp: float64) float64;
    func __cx_log(x: float64) float64;
    func __cx_log2(x: float64) float64;
    func __cx_log10(x: float64) float64;
    func __cx_exp(x: float64) float64;
    func __cx_sin(x: float64) float64;
    func __cx_cos(x: float64) float64;
    func __cx_tan(x: float64) float64;
    func __cx_atan2(y: float64, x: float64) float64;
    func __cx_floor(x: float64) float64;
    func __cx_ceil(x: float64) float64;
    func __cx_round(x: float64) float64;
    func __cx_fabs(x: float64) float64;
    func __cx_fmod(x: float64, y: float64) float64;
}

export let PI: float64 = 3.141592653589793;
export let E: float64 = 2.718281828459045;
export let INF: float64 = 1.0 / 0.0;
export let NAN: float64 = 0.0 / 0.0;

export func abs(x: float64) float64 {
    return __cx_fabs(x);
}

export func floor(x: float64) float64 {
    return __cx_floor(x);
}

export func ceil(x: float64) float64 {
    return __cx_ceil(x);
}

export func round(x: float64) float64 {
    return __cx_round(x);
}

export func min(a: float64, b: float64) float64 {
    if a < b {
        return a;
    }
    return b;
}

export func max(a: float64, b: float64) float64 {
    if a > b {
        return a;
    }
    return b;
}

export func clamp(x: float64, lo: float64, hi: float64) float64 {
    if x < lo {
        return lo;
    }
    if x > hi {
        return hi;
    }
    return x;
}

export func sqrt(x: float64) float64 {
    return __cx_sqrt(x);
}

export func pow(base: float64, exp: float64) float64 {
    return __cx_pow(base, exp);
}

export func log(x: float64) float64 {
    return __cx_log(x);
}

export func log2(x: float64) float64 {
    return __cx_log2(x);
}

export func log10(x: float64) float64 {
    return __cx_log10(x);
}

export func exp(x: float64) float64 {
    return __cx_exp(x);
}

export func sin(x: float64) float64 {
    return __cx_sin(x);
}

export func cos(x: float64) float64 {
    return __cx_cos(x);
}

export func tan(x: float64) float64 {
    return __cx_tan(x);
}

export func atan2(y: float64, x: float64) float64 {
    return __cx_atan2(y, x);
}

export func mod(x: float64, y: float64) float64 {
    return __cx_fmod(x, y);
}

export func is_nan(x: float64) bool {
    return x != x;
}

export func is_inf(x: float64) bool {
    return x == INF || x == -INF;
}

