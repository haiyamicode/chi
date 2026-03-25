import "std/json" as json;

struct Version {
    major: uint32 = 0;
    minor: uint32 = 0;
}

struct Config {
    name: string = "";
    version: Version = {};
    alias: ?string = null;
    latest: ?Version = null;
    history: Array<Version> = [];
    numbers: Array<int> = [];
    active: bool = false;
    count: int = 0;
    ratio: float64 = 0.0;
}

func main() {
    println("=== json.parse object ===");
    var obj = json.parse_raw("{\"name\": \"chi\", \"version\": 1, \"active\": true}");
    println(obj.is_object());
    println(obj.get("name").to_string());
    println(obj.get("version").to_int());
    println(obj.get("active").to_bool());

    println("=== json.parse array ===");
    var arr = json.parse_raw("[1, 2, 3]");
    println(arr.is_array());
    println(arr.length());
    println(arr.at(0).to_int());
    println(arr.at(2).to_int());

    println("=== type checks ===");
    var null_val = json.parse_raw("null");
    println(null_val.is_null());
    var str_val = json.parse_raw("\"hello\"");
    println(str_val.is_string());
    println(str_val.to_string());
    var bool_val = json.parse_raw("false");
    println(bool_val.is_bool());
    println(bool_val.to_bool());

    println("=== has ===");
    println(obj.has("name"));
    println(obj.has("missing"));

    println("=== to_array ===");
    var items = arr.to_array();
    println(items.length);

    println("=== kind display ===");
    printf("{}\n", obj.kind);
    printf("{}\n", arr.kind);
    printf("{}\n", null_val.kind);

    println("=== typed parse ===");
    let cfg2 = json.parse<Config>(
        "{\"name\": \"chi\", \"version\": {\"major\": 1, \"minor\": 7}, \"alias\": \"chi-lang\", \"latest\": {\"major\": 2, \"minor\": 0}, \"history\": [{\"major\": 0, \"minor\": 9}, {\"major\": 1, \"minor\": 0}], \"numbers\": [3, 5, 8], \"active\": true, \"count\": 42, \"ratio\": 2.5}"
    );
    printf("typed name={}\n", cfg2.name);
    printf("typed version={}.{}\n", cfg2.version.major, cfg2.version.minor);

    println("=== typed parse jsonc ===");
    let cfg_jsonc = json.parse<Config>(
        "{\n// config for chi\n\"name\": \"chi\",\n\"version\": {\"major\": 1, \"minor\": 7},\n\"alias\": \"chi-lang\",\n\"latest\": {\"major\": 2, \"minor\": 0},\n\"history\": [{\"major\": 0, \"minor\": 9}, {\"major\": 1, \"minor\": 0},],\n\"numbers\": [3, 5, 8,],\n\"active\": true,\n\"count\": 42,\n\"ratio\": 2.5,\n}"
    );
    printf("jsonc name={}\n", cfg_jsonc.name);
    printf("jsonc numbers={}\n", cfg_jsonc.numbers);

    let non_comment_options = json.ParseOptions{jsonc: false};
    println("=== non-comment parse catch ===");
    try json.parse<Config>(
        "{\n// non-comment mode should reject this\n\"name\": \"chi\",\n}",
        non_comment_options
    ) catch json.ParseError as err {
        printf("non-comment detail={}\n", err.detail);
        if let location = err.location {
            printf("non-comment line={}\n", location.line);
            printf("non-comment column={}\n", location.column);
            printf("non-comment offset={}\n", location.offset);
        }
        printf("non-comment error={}\n", err.message());
    };

    println("=== non-comment parse ===");
    let non_comment_cfg = json.parse<Config>(
        "{\"name\": \"chi\", \"version\": {\"major\": 1, \"minor\": 7}}",
        non_comment_options
    );
    printf("non-comment name={}\n", non_comment_cfg.name);

    println("=== typed parse field error ===");
    try json.parse<Config>("{\"version\": {\"major\": \"oops\", \"minor\": 7}}")
    catch json.ParseError as err {
        printf("field detail={}\n", err.detail);
        if let path = err.path {
            printf("field path={}\n", path);
        }
        printf("field error={}\n", err.message());
    };

    println("=== parse_into ===");
    var cfg = Config{};
    json.parse_into(
        "{\"name\": \"chi\", \"version\": {\"major\": 1, \"minor\": 7}, \"alias\": \"chi-lang\", \"latest\": {\"major\": 2, \"minor\": 0}, \"history\": [{\"major\": 0, \"minor\": 9}, {\"major\": 1, \"minor\": 0}], \"numbers\": [3, 5, 8], \"active\": true, \"count\": 42, \"ratio\": 2.5}",
        &cfg
    );
    printf("name={}\n", cfg.name);
    printf("version={}.{}\n", cfg.version.major, cfg.version.minor);
    if let alias = cfg.alias {
        printf("alias={}\n", alias);
    }
    if let latest = cfg.latest {
        printf("latest={}.{}\n", latest.major, latest.minor);
    }
    printf("history.length={}\n", cfg.history.length);
    printf("history[0]={}.{}\n", cfg.history[0].major, cfg.history[0].minor);
    printf("history[1]={}.{}\n", cfg.history[1].major, cfg.history[1].minor);
    printf("numbers={}\n", cfg.numbers);
    printf("active={}\n", cfg.active);
    printf("count={}\n", cfg.count);
    printf("ratio={}\n", cfg.ratio);

    println("done");
}
