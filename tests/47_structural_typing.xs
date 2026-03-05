// Test: structural typing fallback for interfaces
// A struct that impl's one interface automatically satisfies another
// interface with the same required method signatures.

private interface Sink {
    func write(data: []byte);
}

interface Logger {
    func write(data: []byte);
}

struct ByteCounter {
    count: uint32 = 0;

    impl Sink {
        mut func write(data: []byte) {
            this.count = this.count + data.length;
        }
    }
}

func write_msg(w: &Logger, msg: string) {
    w.write(msg.byte_span());
}

func main() {
    var bc = ByteCounter{};

    // Direct call via structural impl
    bc.write("hello".byte_span());
    printf("count after direct: {}\n", bc.count);

    // Assign to &Logger via structural typing fallback
    var lg: &Logger = &bc;
    lg.write(" world".byte_span());
    printf("count after logger: {}\n", bc.count);

    // Pass as &Logger to a function
    write_msg(&bc, "!!!");
    printf("count after fn: {}\n", bc.count);
}

