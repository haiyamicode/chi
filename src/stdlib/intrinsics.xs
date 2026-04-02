extern "C" {
    unsafe func __copy(dest: *void, src: *void, destruct_old: bool);
    unsafe func __move(dest: *void, src: *void, size: uint32);
    unsafe func __destroy(ptr: *void);
    unsafe func __atomic_load(ptr: *void, result: *void);
    unsafe func __atomic_store(ptr: *void, value: *void);
    unsafe func __atomic_compare_exchange(
        ptr: *void,
        expected: *void,
        desired: *void,
        old_value: *void,
        ok: *bool
    );
    unsafe func __atomic_fetch_add(ptr: *void, value: *void, old_value: *void);
    unsafe func __atomic_fetch_sub(ptr: *void, value: *void, old_value: *void);
    unsafe func __reflect_dyn_elem(parent: *void, value: *void) *void;
}
