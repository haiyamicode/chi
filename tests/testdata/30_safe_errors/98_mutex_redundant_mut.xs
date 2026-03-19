// expect-error: implies mutability

struct Buf {
    mutex mut func grow() {}
}

func main() {}
