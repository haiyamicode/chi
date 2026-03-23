import "std/args" as args;

func main() {
    var cli = args.Command{name: "chi"};
    cli.command({name: "build"});
    cli.command({name: "build"});
}
