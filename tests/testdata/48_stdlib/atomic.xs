import "std/atomic" as atomic;

func main() {
    var value = atomic.Atomic<int32>.from_value(1);

    printf("load0 = {}\n", value.load());

    value.store(2);
    printf("load1 = {}\n", value.load());

    let (old1, ok1) = value.compare_exchange(2, 5);
    printf("cmp1 old={} ok={}\n", old1, ok1);
    printf("load2 = {}\n", value.load());

    let (old2, ok2) = value.compare_exchange(2, 9);
    printf("cmp2 old={} ok={}\n", old2, ok2);
    printf("load3 = {}\n", value.load());

    let old3 = value.fetch_add(3);
    printf("add old={}\n", old3);
    printf("load4 = {}\n", value.load());

    let old4 = value.fetch_sub(2);
    printf("sub old={}\n", old4);
    printf("load5 = {}\n", value.load());
}
