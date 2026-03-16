// Union type in managed mode.
// expect-error: 'union' types are not allowed in safe mode

union Data {
    i: int;
    f: float;
}

func main() {
    var d = Data{i: 42};
}
