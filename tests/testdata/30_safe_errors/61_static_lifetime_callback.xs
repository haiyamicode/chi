// Lambda capturing local reference passed to func<'static> — must be rejected.
// expect-error: does not live long enough

import "std/time" as time;

struct Counter {
    value: int;
}

func main() {
    var c = Counter{value: 42};
    var ref: &Counter = &c;

    time.timeout(50, func [ref] () {
        printf("timeout: counter = {}\n", ref.value);
    });
}
