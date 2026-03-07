// std/time — time utilities

extern "C" {
    func __cx_time_now() uint64;
    func __cx_time_monotonic() uint64;
    unsafe func cx_timeout(delay: uint64, callback: *void);
}

export func now() uint64 {
    return __cx_time_now();
}

export func monotonic() uint64 {
    return __cx_time_monotonic();
}

export func timeout(delay: uint64, callback: func<'static>) {
    unsafe {
        cx_timeout(delay, &callback);
    }
}

export func sleep(ms: uint64) Promise {
    return Promise.make(
        func (resolve) {
            timeout(ms, func [resolve] () {
                resolve(());
            });
        }
    );
}

