import "std/args" as args;
import "std/fs" as fs;

func main() {
    let values = args.argv();
    printf("argc = {}\n", values.length);
    println(fs.exists("."));
}
