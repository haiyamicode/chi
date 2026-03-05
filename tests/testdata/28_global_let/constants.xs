export let magic_number = 42;
export let pi_approx = 3;
export let module_greeting = "hello from module";

export func compute_value() int {
    return 100 + 23;
}

export let computed = compute_value();
export let derived = magic_number * 2;
