import "std/args" as args;

func main() {
    var cli = args.Command{name: "chi"};
    cli.flag({name: "verbose", short: "v"});
    cli.flag({name: "verbose", short: "V"});
}
