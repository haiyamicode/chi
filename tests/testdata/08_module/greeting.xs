export * from "./subgreeting";

struct Greeting {
    name: string;

    mut func new(name: string) {
        this.name = name;
    }

    func hello() {
        printf("hello, {}\n", this.name);
    }
}

func hello() {
    println("hello from module");
}

