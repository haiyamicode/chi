enum Parse<T, E> {
    Ok(T),
    Error(E)
}

struct Wrap {
    result: Parse<int, string>;
}

func accept_parse(value: Parse<int, string>) {}

func make_ok() Parse<int, string> {
    return Parse<int, string>.Ok{1};
}

func main() {
    var direct: Parse<int, string> = Parse<int, string>.Ok{1};
    var wrap = Wrap{result: Parse<int, string>.Error{"field"}};
    accept_parse(Parse<int, string>.Ok{2});
    var arr: Array<Parse<int, string>> = [
        Parse<int, string>.Ok{3},
        Parse<int, string>.Error{"array"}
    ];

    var picked = switch direct {
        Ok => Parse<int, string>.Ok{10},
        Error => Parse<int, string>.Error{"bad"}
    };
}
