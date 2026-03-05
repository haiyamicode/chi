import "std/json" as json;

func main() {
    println("=== json.parse object ===");
    var obj = json.parse("{\"name\": \"chi\", \"version\": 1, \"active\": true}");
    println(obj.is_object());
    println(obj.get("name").to_string());
    println(obj.get("version").to_int());
    println(obj.get("active").to_bool());

    println("=== json.parse array ===");
    var arr = json.parse("[1, 2, 3]");
    println(arr.is_array());
    println(arr.length());
    println(arr.at(0).to_int());
    println(arr.at(2).to_int());

    println("=== type checks ===");
    var null_val = json.parse("null");
    println(null_val.is_null());
    var str_val = json.parse("\"hello\"");
    println(str_val.is_string());
    println(str_val.to_string());
    var bool_val = json.parse("false");
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

    println("done");
}

