// Raw pointer dereference outside unsafe block.
// expect-error: raw pointer dereference requires unsafe block

import "std/mem" as mem;

func main() {
    var x: int = 42;
    var p: *int = &x;
    var v = p!;
}

