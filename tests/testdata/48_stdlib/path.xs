import "std/path" as path;

func main() {
    printf("dir('/a/b/c.txt') = {}\n", path.dir("/a/b/c.txt"));
    printf("dir('file.txt') = {}\n", path.dir("file.txt"));
    printf("dir('/file.txt') = {}\n", path.dir("/file.txt"));
    printf("base('/a/b/c.txt') = {}\n", path.base("/a/b/c.txt"));
    printf("base('/a/b/') = {}\n", path.base("/a/b/"));
    printf("base('') = {}\n", path.base(""));
    printf("ext('/a/b/c.txt') = {}\n", path.ext("/a/b/c.txt"));
    printf("ext('noext') = {}\n", path.ext("noext"));
    printf("is_absolute('/a') = {}\n", path.is_absolute("/a"));
    printf("is_absolute('a') = {}\n", path.is_absolute("a"));
    printf("join('a', 'b') = {}\n", path.join("a", "b"));
    printf("join('a/', 'b') = {}\n", path.join("a/", "b"));
    printf("join('', 'b') = {}\n", path.join("", "b"));
    printf("join('a', '') = {}\n", path.join("a", ""));
}

