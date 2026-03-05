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
    // f4 still valid (Shared refcount keeps FileHandle alive until both drop)

    fs.remove(test_file);

    println("=== fs.exists (nonexistent) ===");
    println(fs.exists("/tmp/chi_fs_test_nonexistent_12345"));

    println("done");
}
