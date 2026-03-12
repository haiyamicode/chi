// Broken contextual enum variants and switch destructuring should stay in recovery mode

enum Result<T, E> {
    Ok(T),
    Err { error: E };
}

func main() {
    var r: Result<int, string> = Ok(;

    switch r {
        Ok(value => println(value);
        Err{error, [first, ...rest]} => println(error);
        MissingVariant{ => {}
        Ok(value) if => println(value)
        else =>
    }

    var s: Result<int, string> = Err{error: "boom",,};
}
