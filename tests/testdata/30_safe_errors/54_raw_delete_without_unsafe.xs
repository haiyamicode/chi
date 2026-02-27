// Delete on raw pointer outside unsafe block.
// expect-error: 'delete' on raw pointer requires unsafe block

import "std/mem" as mem;

func main() {
    var p: *int = null;
    unsafe {
        p = mem.malloc(sizeof int) as *int;
    }
    delete p;
}

