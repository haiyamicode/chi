// expect-error: if used as an expression must have an else and every branch must produce a value

func main() {
    let x = if true => 1;
    println(x);
}
