// Cannot move from an if-let alias because it does not own storage.
// expect-error: cannot move from non-owning alias

func main() {
    let value: ?string = "hello";
    if let item = value {
        let moved = move item;
        println(moved);
    }
}
