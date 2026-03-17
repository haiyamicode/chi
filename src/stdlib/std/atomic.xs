import "std/ops" as ops;

extern "C" {
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
}

export struct Atomic<T: ops.Int> {
    private value: T;

    static func from_value(value: T) Atomic<T> {
        var result: Atomic<T> = undefined;
        result.value = value;
        return result;
    }

    func load() T {
        unsafe {
            var result: T = undefined;
            __atomic_load(&this.value as *T as *void, &result as *T as *void);
            return move result;
        }
    }

    mut func store(value: T) {
        unsafe {
            __atomic_store(&mut this.value as *T as *void, &value as *T as *void);
        }
    }

    mut func compare_exchange(expected: T, desired: T) Tuple<T, bool> {
        unsafe {
            var old_value: T = undefined;
            var ok = false;
            __atomic_compare_exchange(
                &mut this.value as *T as *void,
                &expected as *T as *void,
                &desired as *T as *void,
                &old_value as *T as *void,
                &ok
            );
            return (move old_value, ok);
        }
    }

    mut func fetch_add(value: T) T {
        unsafe {
            var old_value: T = undefined;
            __atomic_fetch_add(
                &mut this.value as *T as *void,
                &value as *T as *void,
                &old_value as *T as *void
            );
            return move old_value;
        }
    }

    mut func fetch_sub(value: T) T {
        unsafe {
            var old_value: T = undefined;
            __atomic_fetch_sub(
                &mut this.value as *T as *void,
                &value as *T as *void,
                &old_value as *T as *void
            );
            return move old_value;
        }
    }

    impl ops.NoCopy {}
}
