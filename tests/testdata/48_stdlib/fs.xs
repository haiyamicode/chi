import "std/fs" as fs;

func main() {
    let test_dir = "/tmp/chi_fs_test";
    let test_file = "/tmp/chi_fs_test/hello.txt";

    println("=== fs.mkdir ===");
    fs.mkdir_all(test_dir);
    println(fs.exists(test_dir));

    println("=== File class ===");
    var f = fs.File.create(test_file);
    f.write_string("hello world");
    f.close();
    println(fs.exists(test_file));

    var f2 = fs.File.open(test_file);
    println(f2.read_string());
    f2.close();

    println("=== fs.read_file / write_file ===");
    fs.write_file(test_file, "overwritten");
    println(fs.read_file(test_file));

    println("=== fs.append_file ===");
    fs.append_file(test_file, "!");
    println(fs.read_file(test_file));

    println("=== fs.list_dir ===");
    let entries = fs.list_dir(test_dir);
    println(entries.length);
    println(entries[0]);

    println("=== fs.remove ===");
    fs.remove(test_file);
    println(fs.exists(test_file));

    println("=== File copy ===");
    fs.write_file(test_file, "shared handle");
    var f3 = fs.File.open(test_file);
    var f4 = f3; // copy — both share the same underlying handle
    println(f4.read_string());
    f3.close(); // close via original

    fs.remove(test_file);

    println("=== fs.exists (nonexistent) ===");
    println(fs.exists("/tmp/chi_fs_test_nonexistent_12345"));

    println("=== error: open nonexistent ===");
    try fs.File.open("/tmp/chi_no_such_file_12345") catch fs.FsError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
        printf("path: {}\n", err.path);
        printf("raw_code: {}\n", err.raw_code);
        printf("has detail: {}\n", err.detail.byte_length() > 0);
        printf("message: {}\n", err.message());
    };

    println("=== error: remove nonexistent ===");
    try fs.remove("/tmp/chi_no_such_file_12345") catch fs.FsError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
    };

    println("=== error: list_dir nonexistent ===");
    try fs.list_dir("/tmp/chi_no_such_dir_12345") catch fs.FsError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
    };

    println("=== error: result mode ===");
    var result = try fs.File.open("/tmp/chi_no_such_file_12345") catch fs.FsError;
    switch result {
        Err(err) => {
            var e = err.as_ref();
            switch e.(type) {
                &fs.FsError => {
                    printf("kind: {}\n", e.kind);
                    printf("op: {}\n", e.op);
                },
                else => {}
            }
        },
        else => {}
    }

    println("done");
}

