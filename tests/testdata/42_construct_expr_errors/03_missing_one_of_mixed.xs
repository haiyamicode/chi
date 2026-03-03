// Only defaulted field provided, required field missing
// expect-error: missing field 'name'
struct Config {
    name: string;
    value: int = 0;
}

func main() {
    var c = Config{value: 42};
}

