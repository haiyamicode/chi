import "std/args" as args;

func main() {
    var cli = args.Command{name: "chi"};
    cli.option({name: "output", short: "o"});
    cli.option({name: "output", short: "p"});
}
