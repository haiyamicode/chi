import "std/ops" as ops;

extern "C" {
    unsafe func cx_malloc(size: uint32, ignored: *void) *void;
    unsafe func cx_free(address: *void);
    unsafe func cx_memset(address: *void, v: uint8, n: uint32);
    unsafe func __copy(dest: *void, src: *void, destruct_old: bool);
    unsafe func __move(dest: *void, src: *void, size: uint32);
}















export extern "C" {
    func memcmp(s1: *void, s2: *void, n: uint32) int;
}







export unsafe func malloc(size: uint32) *void {
    return cx_malloc(size, null);
}

export unsafe func alloc<T>() &move T {
    return cx_malloc(sizeof T, null) as *T as &move T;
}

export func copy<T: ops.Unsized>(val: &T) &move T {
    unsafe {
        var ref = cx_malloc(sizeof val, null) as *T as &move T;
        __copy(ref as *T, val as *T, false);
        return ref;
    }
}

export unsafe func free(address: *void) {
    cx_free(address);
}

export unsafe func memset(address: *void, v: uint8, n: uint32) {
    cx_memset(address, v, n);
}

export unsafe func write<T>(dest: *T, value: T) {
    __move(dest as *void, &value, sizeof T);
}
