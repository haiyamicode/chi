import "std/fs" as fs;

async func run_async_wrapper_cases() Promise {
    println("=== File.async().read_string ===");
    fs.write_file("/tmp/chi_fs_stdlib_test/async.txt", "async hello");
    println(await fs.read_file_async("/tmp/chi_fs_stdlib_test/async.txt"));
    fs.remove("/tmp/chi_fs_stdlib_test/async.txt");

    println("=== fs.write_file_async ===");
    await fs.write_file_async("/tmp/chi_fs_stdlib_test/async_write.txt", "async write");
    println(fs.read_file("/tmp/chi_fs_stdlib_test/async_write.txt"));
    fs.remove("/tmp/chi_fs_stdlib_test/async_write.txt");

    println("=== fs.append_file_async ===");
    fs.write_file("/tmp/chi_fs_stdlib_test/async_append.txt", "prefix");
    await fs.append_file_async("/tmp/chi_fs_stdlib_test/async_append.txt", "!");
    println(fs.read_file("/tmp/chi_fs_stdlib_test/async_append.txt"));
    fs.remove("/tmp/chi_fs_stdlib_test/async_append.txt");
    return ();
}

func main() {
    let test_dir = "/tmp/chi_fs_stdlib_test";
    let test_file = "/tmp/chi_fs_stdlib_test/hello.txt";

    println("=== fs.mkdir ===");
    fs.mkdir_all(test_dir);
    if fs.exists("/tmp/chi_fs_stdlib_test/async.txt") {
        fs.remove("/tmp/chi_fs_stdlib_test/async.txt");
    }
    if fs.exists("/tmp/chi_fs_stdlib_test/async_write.txt") {
        fs.remove("/tmp/chi_fs_stdlib_test/async_write.txt");
    }
    if fs.exists("/tmp/chi_fs_stdlib_test/async_append.txt") {
        fs.remove("/tmp/chi_fs_stdlib_test/async_append.txt");
    }
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

    println("=== fs.remove_all / copy_all ===");
    let source_tree = "/tmp/chi_fs_stdlib_test_src";
    let copied_tree = "/tmp/chi_fs_stdlib_test_copy";
    fs.remove_all(source_tree);
    fs.remove_all(copied_tree);
    fs.mkdir_all(source_tree + "/nested");
    fs.write_file(source_tree + "/nested/data.txt", "copied");
    fs.copy_all(source_tree, copied_tree);
    println(fs.read_file(copied_tree + "/nested/data.txt"));
    fs.remove_all(copied_tree);
    println(fs.exists(copied_tree));
    fs.remove_all(source_tree);

    println("=== fs.glob ===");
    let glob_root = "/tmp/chi_fs_glob_test";
    fs.remove_all(glob_root);
    fs.mkdir_all(glob_root + "/nested");
    fs.write_file(glob_root + "/a.txt", "a");
    fs.write_file(glob_root + "/nested/b.txt", "b");
    fs.write_file(glob_root + "/nested/c.h", "c");
    let root_txt = fs.glob("*.txt", glob_root);
    println(root_txt.length);
    println(root_txt[0]);
    let nested_txt = fs.glob("nested/*.txt", glob_root);
    println(nested_txt.length);
    println(nested_txt[0]);
    let recursive_txt = fs.glob("**/*.txt", glob_root);
    println(recursive_txt.length);
    println(recursive_txt[0]);
    println(recursive_txt[1]);
    fs.remove_all(glob_root);

    println("=== File copy ===");
    fs.write_file(test_file, "shared handle");
    var f3 = fs.File.open(test_file);
    var f4 = f3; // copy — both share the same underlying handle
    println(f4.read_string());
    f3.close(); // close via original

    fs.remove(test_file);

    println("=== fs.exists (nonexistent) ===");
    println(fs.exists("/tmp/chi_fs_stdlib_test_nonexistent_12345"));

    println("=== error: open nonexistent ===");
    try fs.File.open("/tmp/chi_no_such_file_12345") catch fs.FileError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
        printf("path: {}\n", err.path);
        printf("raw_code: {}\n", err.raw_code);
        printf("has detail: {}\n", err.detail.byte_length() > 0);
        printf("message: {}\n", err.message());
    };

    println("=== error: remove nonexistent ===");
    try fs.remove("/tmp/chi_no_such_file_12345") catch fs.FileError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
    };

    println("=== error: list_dir nonexistent ===");
    try fs.list_dir("/tmp/chi_no_such_dir_12345") catch fs.FileError as err {
        printf("kind: {}\n", err.kind);
        printf("op: {}\n", err.op);
    };

    println("=== error: result mode ===");
    var result = try fs.File.open("/tmp/chi_no_such_file_12345") catch fs.FileError;
    switch result {
        Err(err) => {
            var e = err.as_ref();
            switch e.(type) {
                &fs.FileError => {
                    printf("kind: {}\n", e.kind);
                    printf("op: {}\n", e.op);
                },
                else => {}
            }
        },
        else => {}
    }

    run_async_wrapper_cases();

    println("done");
}
