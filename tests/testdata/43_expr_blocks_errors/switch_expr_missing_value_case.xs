// expect-error: switch used as an expression must have every case produce a value

func main() {
    let x = switch 1 {
        1 => 1,
        else => {
        },
    };
    println(x);
}
