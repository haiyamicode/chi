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

func main() {
    // Local module-level let
    printf("answer = {}\n", answer);
    printf("greeting = {}\n", greeting);
    printf("computed = {}\n", computed);
    printf("derived = {}\n", derived);

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

