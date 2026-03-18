import "std/args" as args;

func main() {
    let argv = args.argv();
    printf("argc = {}\n", argv.length);
    printf("has exe = {}\n", argv.length > 0);
    if argv.length > 0 {
        printf("exe contains test name = {}\n", argv[0].contains("48_stdlib_args"));
    }
}
