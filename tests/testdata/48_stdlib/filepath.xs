import "std/filepath" as filepath;

func main() {
    println("=== filepath.join ===");
    println(filepath.join("/tmp", "chi", "src"));

    println("=== filepath.dir ===");
    println(filepath.dir("/tmp/chi/main.xs"));

    println("=== filepath.base ===");
    println(filepath.base("/tmp/chi/main.xs"));

    println("=== filepath.ext ===");
    println(filepath.ext("/tmp/chi/main.xs"));

    println("=== filepath.is_absolute ===");
    println(filepath.is_absolute("/tmp/chi"));
}
