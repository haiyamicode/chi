// expect-panic: panic: unhandled typed throw

struct MyError {
    msg: string = "";

    impl Error {
        func message() string {
            return this.msg;
        }
    }
}

func main() {
    throw new MyError{msg: "unhandled typed throw"};
}
