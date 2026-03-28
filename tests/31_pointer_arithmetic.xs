import "std/mem" as mem;

struct PtrPoint {
    value: int = 0;
}

func test_ptr_add_sub() {
    println("=== Test: ptr + n, ptr - n ===");
    var arr: Array<int> = [10, 20, 30, 40, 50];
    unsafe {
        var p = &arr[0] as *int;
        printf("*p={}\n", *p);
        var p2 = p + 2;
        printf("*(p+2)={}\n", *p2);
        var p3 = p2 - 1;
        printf("*(p+2-1)={}\n", *p3);
        var p4 = 3 + p;
        printf("*(3+p)={}\n", *p4);
    }
}

func test_ptr_diff() {
    println("=== Test: ptr - ptr ===");
    var arr: Array<int> = [10, 20, 30, 40, 50];
    unsafe {
        var p0 = &arr[0] as *int;
        var p3 = p0 + 3;
        var diff = p3 - p0;
        printf("diff={}\n", diff);
        var p1 = p0 + 1;
        printf("p3-p1={}\n", p3 - p1);
    }
}

func test_ptr_inc_dec() {
    println("=== Test: ptr++, ptr-- ===");
    var arr: Array<int> = [100, 200, 300];
    unsafe {
        var p = &arr[0] as *int;
        printf("*p={}\n", *p);
        p++;
        printf("after p++: *p={}\n", *p);
        p++;
        printf("after p++: *p={}\n", *p);
        p--;
        printf("after p--: *p={}\n", *p);
    }
}

func test_ptr_prefix_inc_dec() {
    println("=== Test: ++ptr, --ptr ===");
    var arr: Array<int> = [10, 20, 30];
    unsafe {
        var p = &arr[0] as *int;
        var val = *++p;
        printf("*++p={}\n", val);
        val = *--p;
        printf("*--p={}\n", val);
    }
}

func test_ptr_compare() {
    println("=== Test: ptr comparisons ===");
    var arr: Array<int> = [1, 2, 3];
    unsafe {
        var p0 = &arr[0] as *int;
        var p2 = p0 + 2;
        printf("p0 < p2: {}\n", p0 < p2);
        printf("p2 > p0: {}\n", p2 > p0);
        printf("p0 >= p0: {}\n", p0 >= p0);
        printf("p0 <= p2: {}\n", p0 <= p2);
        printf("p0 == p0: {}\n", p0 == p0);
        printf("p0 != p2: {}\n", p0 != p2);
    }
}

func test_inttoptr_ptrtoint() {
    println("=== Test: int<->ptr casts ===");
    var x: int = 42;
    unsafe {
        var p = &x as *int;
        var addr = p as uint64;
        var p2 = addr as *int;
        printf("*p2={}\n", *p2);
        printf("roundtrip: {}\n", p == p2);
    }
}

func test_malloc_ptr_arith() {
    println("=== Test: malloc + ptr arith ===");
    unsafe {
        var buf = mem.malloc(5 * sizeof int) as *int;
        var i: int = 0;
        while i < 5 {
            *(buf + i) = i * 10;
            i++;
        }
        var p = buf;
        i = 0;
        while i < 5 {
            printf("{} ", *p);
            p++;
            i++;
        }
        printf("\n");
        mem.free(buf as *void);
    }
}

func test_ptr_indexing() {
    println("=== Test: ptr[i] ===");
    var arr: Array<int> = [5, 10, 15, 20];
    unsafe {
        var p = &arr[0] as *int;
        printf("p[0]={}, p[2]={}, p[3]={}\n", p[0], p[2], p[3]);
    }
}

func test_ptr_null_compare() {
    println("=== Test: ptr null comparisons ===");
    var x: int = 42;
    unsafe {
        var p: *int = &x;
        printf("p != null: {}\n", p != null);
        printf("p == null: {}\n", p == null);
        printf("null != p: {}\n", null != p);
        printf("null == p: {}\n", null == p);
    }
}

func test_ptr_member_access() {
    println("=== Test: ptr member access in unsafe ===");
    var point = PtrPoint{value: 99};
    unsafe {
        var p = &point as *PtrPoint;
        printf("p.value={}\n", p.value);
        p.value = 123;
        printf("after write: point.value={}\n", point.value);
    }
}

func main() {
    test_ptr_add_sub();
    test_ptr_diff();
    test_ptr_inc_dec();
    test_ptr_prefix_inc_dec();
    test_ptr_compare();
    test_inttoptr_ptrtoint();
    test_malloc_ptr_arith();
    test_ptr_indexing();
    test_ptr_null_compare();
    test_ptr_member_access();
}
