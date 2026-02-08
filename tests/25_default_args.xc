func greet(name: string, greeting: string = "Hello") {
    printf("{}, {}!\n", greeting, name);
}

func format_number(n: int, width: int = 0, fill: char = ' ') int {
    printf("n={}, width={}, fill={}\n", n, width, fill);
    return n;
}

func add_offset(x: int, offset: int = 10 * 2) int {
    return x + offset;
}

struct Greeter {
    prefix: string;

    func new(prefix: string = "Hey") {
        this.prefix = prefix;
    }

    func say(name: string, suffix: string = "!") {
        printf("{} {}{}\n", this.prefix, name, suffix);
    }
}

func main() {
    greet("Alice");
    greet("Bob", "Hi");
    format_number(42);
    format_number(42, 5);
    format_number(42, 5, '0');
    printf("add_offset(5) = {}\n", add_offset(5));
    printf("add_offset(5, 100) = {}\n", add_offset(5, 100));
    var g1: Greeter = {};
    g1.say("World");
    g1.say("World", "?");
    var g2: Greeter = {"Hello"};
    g2.say("Chi");
}

