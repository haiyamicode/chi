import "std/time" as time;

func main() {
    println("=== time.now ===");
    let t = time.now();
    // Should be a reasonable timestamp (after 2024-01-01 = 1704067200000)
    println(t > 1704067200000);

    println("=== time.monotonic ===");
    let m1 = time.monotonic();
    let m2 = time.monotonic();
    // monotonic should be increasing
    println(m2 >= m1);

    println("=== time.timeout ===");
    time.timeout(1, func () {
        println("timeout fired");
    });

    println("=== time.sleep ===");
    time.sleep(1).then(func (u) {
        println("sleep resolved");
    });

    println("done");
}
