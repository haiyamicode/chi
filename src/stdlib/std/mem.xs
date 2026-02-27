import "std/ops" as ops;

extern "C" {
    private unsafe func cx_malloc(size: uint32, ignored: *void) *void;
    private unsafe func cx_free(address: *void);
    private unsafe func cx_memset(address: *void, v: uint8, n: uint32);
    private unsafe func __copy_from(dest: *void, src: *void, destruct_old: bool);
}

unsafe func malloc(size: uint32) *void {
    return cx_malloc(size, null);
}

unsafe func alloc<T>() &move T {
    return cx_malloc(sizeof T, null) as *T as &move T;
}

func copy_from<T: ops.AllowUnsized>(val: &T) &move T {
    unsafe {
        var ref = cx_malloc(sizeof val, null) as *T as &move T;
        __copy_from(ref as *T, val as *T, false);
        return ref;
    }
}

unsafe func free(address: *void) {
    cx_free(address);
}

unsafe func memset(address: *void, v: uint8, n: uint32) {
    cx_memset(address, v, n);
}

