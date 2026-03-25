// expect-panic: panic: JSON parse error: syntax error

import "std/json" as json;

struct Config {
    name: string = "";
}

func main() {
    let _cfg = json.parse<Config>(
        "{\n// comment\n\"name\": \"chi\",\n}",
        json.ParseOptions{jsonc: false}
    );
}
