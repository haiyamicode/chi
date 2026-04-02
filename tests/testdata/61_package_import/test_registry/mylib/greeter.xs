export struct Greeter {
    name: string;

    mut func new(name: string) {
        this.name = name;
    }

    func greet() {
        printf("hello from {}\n", this.name);
    }
}

export func default_greeting() {
    println("hello from mylib");
}
