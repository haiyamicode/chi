func print_view(s: []int) {
    for item in s {
        printf("{} ", item);
    }
    println("");
}

func sum(s: []int) int {
    var total = 0;
    for item in s {
        total = total + item;
    }
    return total;
}

func main() {
    var arr: Array<int> = [1, 2, 3, 4, 5];

    // as_array_view
    var s = arr.as_array_view();
    printf("len: {}\n", s.length);
    printf("is_empty: {}\n", s.is_empty());

    // for loop iteration
    print_view(s);

    // Display
    printf("{}\n", s);

    // index operator
    printf("s[0]: {}\n", s[0]);
    printf("s[2]: {}\n", s[2]);
    printf("s[4]: {}\n", s[4]);

    // index mutation
    s[2] = 30;
    printf("after s[2]=30: {}\n", s);

    // for loop with index
    for item, i in s {
        printf("{}:{} ", i, item);
    }
    println("");

    // view from Array
    var v1 = arr.view(1, 4);
    printf("view(1,4): {}\n", v1);

    var v2 = arr.view(null, 3);
    printf("view(null,3): {}\n", v2);

    var v3 = arr.view(2, null);
    printf("view(2,null): {}\n", v3);

    // slice on array view (produces another []T)
    var sub = s.slice(1, 4);
    printf("slice(1,4): {}\n", sub);

    var sub2 = s.slice(null, 2);
    printf("slice(null,2): {}\n", sub2);

    var sub3 = s.slice(3, null);
    printf("slice(3,null): {}\n", sub3);

    // slice operator on array view
    var sub4 = s[1..4];
    printf("s[1..4]: {}\n", sub4);

    // pass to function
    printf("sum: {}\n", sum(s));

    // empty view
    var empty_arr: Array<int> = [];
    var ev = empty_arr.as_array_view();
    printf("empty len: {}\n", ev.length);
    printf("empty is_empty: {}\n", ev.is_empty());
    printf("empty: {}\n", ev);

    // view reflects underlying array mutation
    s[0] = 100;
    printf("arr[0] after view mutation: {}\n", arr[0]);
}

