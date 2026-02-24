// Unsafe block in managed mode.
// expect-error: 'unsafe' blocks are not allowed in managed mode

func main() {
    unsafe {
        println("nope");
    }
}

