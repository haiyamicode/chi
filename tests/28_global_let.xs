// Test module-level let declarations

import "./testdata/28_global_let" as constants;
import {magic_number, module_greeting} from "./testdata/28_global_let";

let answer = 42;
let greeting = "hello world";

func compute_value() int {
    return 100 + 23;
}

let computed = compute_value();
let derived = answer * 2;

// Module-level let with struct constructor — exercises the deferred-resolution
// pass: the constructor must see fully-resolved struct members.
struct Point {
    x: int = 0;
    y: int = 0;

    func new() {}

    func sum() int {
        return this.x + this.y;
    }
}

let g_point_inferred = Point{x: 3, y: 4};
let g_point_typed = Point{x: 10, y: 20};

struct Container {
    value: int = 0;

    func new() {}

    func read() int {
        return this.value;
    }
}

let g_container = Container{value: 77};

// Method-as-value at module scope: a struct field default references a method
// on a module-level global. The synthesized lambda has no captures because
// the receiver is a global.
struct Caller {
    fn: func () int = g_container.read;
}

func main() {
    // Local module-level let
    printf("answer = {}\n", answer);
    printf("greeting = {}\n", greeting);
    printf("computed = {}\n", computed);
    printf("derived = {}\n", derived);

    // Module-level let with struct constructor
    printf("g_point_inferred.sum = {}\n", g_point_inferred.sum());
    printf("g_point_typed.sum = {}\n", g_point_typed.sum());
    printf("g_container.value = {}\n", g_container.value);

    // Method-as-value at module scope
    var caller = Caller{};
    printf("caller.fn() = {}\n", caller.fn());

    // Imported module-level let via module alias
    printf("constants.magic_number = {}\n", constants.magic_number);
    printf("constants.pi_approx = {}\n", constants.pi_approx);
    printf("constants.module_greeting = {}\n", constants.module_greeting);
    printf("constants.computed = {}\n", constants.computed);
    printf("constants.derived = {}\n", constants.derived);

    // Imported module-level let via named import
    printf("magic_number = {}\n", magic_number);
    printf("module_greeting = {}\n", module_greeting);
}
