func main() {
    var refs: Array<&int> = {};
    var i: int = 0;
    while i < 5 {
        var n = i * 10;
        refs.push(&n);
        i = i + 1;
    }

    // Force GC pressure so any unreachable root would be collected before use.
    var churn: Array<int> = {};
    var j: int = 0;
    while j < 200 {
        churn.push(j);
        j = j + 1;
    }

    for r in refs {
        printf("r = {}\n", *r);
    }
}
