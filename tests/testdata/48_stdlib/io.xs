import "std/io" as io;

// ── io.Write via structural typing ──────────────────────────────────────────

func write_all(w: &io.Write, text: string) {
    w.write_string(text);
}

// ── io.Read via structural typing ──────────────────────────────────────────

func read_all(r: &io.Read) string {
    return r.read_string();
}

func main() {
    // Buffer satisfies io.Write (structural typing via impl Writer)
    var buf = Buffer{};
    var w: &io.Write = &buf;
    write_all(w, "hello ");
    write_all(w, "world");
    printf("writer: {}\n", buf.to_string());

    // Buffer satisfies io.Read (structural typing via impl Reader)
    var src = Buffer.from_string("abcdefghij");
    var r: &io.Read = &src;
    printf("reader: {}\n", read_all(r));

    // io.Read default method: read_bytes
    var src2 = Buffer.from_string("fghij");
    var r2: &io.Read = &src2;
    var chunk = r2.read_bytes(3);
    printf("read_bytes: {}\n", chunk.to_string());

    // io.Read default method: read_all (reads until EOF)
    var src3 = Buffer.from_string("hello");
    var r3: &io.Read = &src3;
    var all = r3.read_all();
    printf("read_all: {}\n", all.to_string());

    // io.Write default method: write_string
    var out = Buffer{};
    var w2: &io.Write = &out;
    w2.write_string("test write_string");
    printf("write_string: {}\n", out.to_string());
}

