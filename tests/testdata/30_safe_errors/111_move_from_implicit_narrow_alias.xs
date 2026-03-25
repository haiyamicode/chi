// Implicit truthy narrowing also creates a non-owning alias.
// expect-error: cannot move from non-owning alias

func main() {
    let value: ?string = "hello";
    if value {
        let moved = move value;
        println(moved);
    }
}
