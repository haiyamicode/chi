// expect-error: if used as an expression must have an else and every branch must produce a value

struct FlagBox {
    enabled: bool = true;
}

func main() {
    let x = if true { FlagBox{} } else { {} };
    println(x.enabled);
}
