private extern "C" {
    func cx_malloc(size: uint32, ignored: *void) *void;
    func cx_free(address: *void);
    func cx_memset(address: *void, v: uint8, n: uint32);
}

unsafe func malloc(size: uint32) *void {
    return cx_malloc(size, null);
}

unsafe func free(address: *void) {
    cx_free(address);
}

unsafe func memset(address: *void, v: uint8, n: uint32) {
    cx_memset(address, v, n);
}
