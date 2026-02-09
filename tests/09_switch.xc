func switch_int() string {
    var levels: Array<int> = [];
    levels.add(1);
    levels.add(2);
    levels.add(3);
    levels.add(4);

    for item in levels {
        var label = switch item {
            1, 2 => "low",
            3 => "medium",
            else => {
                println("default case:");
                "high"
            }
        };
        println(label);
    }

    return switch levels[0] {
        1, 2 => "low",
        3 => "medium",
        else => "high"
    };
}

func main() {
    var result = switch_int();
    printf("result for first level: {}\n", result);
}

