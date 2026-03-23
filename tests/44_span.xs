func print_span(s: &[int]) {
    for item in s {
        printf("{} ", item);
    }
    println("");
}

func sum(s: &[int]) int {
    var total = 0;
    for item in s {
        total = total + item;
    }
    return total;
}

func sum_lt<'a>(s: &'a [int]) int {
    return sum(s);
}

func bump_lt<'a>(s: &(mut, 'a) [int]) {
    s[1] = s[1] + 10;
}

func main() {
    var arr: Array<int> = [1, 2, 3, 4, 5];

    // immutable span
    var s = arr.span();
    printf("len: {}\n", s.length);
    printf("is_empty: {}\n", s.is_empty());

    // for loop iteration
    print_span(s);

    // Display
    printf("{}\n", s);

    // index read
    printf("s[0]: {}\n", s[0]);
    printf("s[2]: {}\n", s[2]);
    printf("s[4]: {}\n", s[4]);

    // mutable span — index mutation
    var ms = arr.span_mut();
    ms[2] = 30;
    printf("after ms[2]=30: {}\n", ms);

    // for loop with index on mutable span
    for item, i in ms {
        printf("{}:{} ", i, item);
    }
    println("");

    // span from Array with bounds
    var v1 = arr.span(1, 4);
    printf("span(1,4): {}\n", v1);

    var v2 = arr.span(null, 3);
    printf("span(null,3): {}\n", v2);

    var v3 = arr.span(2, null);
    printf("span(2,null): {}\n", v3);

    // slice on span (produces another &[T])
    var sub = s.slice(1, 4);
    printf("slice(1,4): {}\n", sub);

    var sub2 = s.slice(null, 2);
    printf("slice(null,2): {}\n", sub2);

    var sub3 = s.slice(3, null);
    printf("slice(3,null): {}\n", sub3);

    // slice operator on span
    var sub4 = s[1..4];
    printf("s[1..4]: {}\n", sub4);

    // pass to function — &mut [T] coerces to &[T]
    printf("sum: {}\n", sum(ms));
    printf("sum_lt: {}\n", sum_lt(s));

    // empty span
    var empty_arr: Array<int> = [];
    var ev = empty_arr.span();
    printf("empty len: {}\n", ev.length);
    printf("empty is_empty: {}\n", ev.is_empty());
    printf("empty: {}\n", ev);

    var arr2: Array<int> = [10, 20, 30];
    bump_lt(arr2.span_mut());
    printf("bump_lt: {}\n", arr2);

    // mutable span reflects underlying array mutation
    ms[0] = 100;
    printf("arr[0] after span mutation: {}\n", arr[0]);
}
