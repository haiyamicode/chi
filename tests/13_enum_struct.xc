enum Node (type: uint64) {
    VarDecl,
    FnDef {
        params: Array<string>;
        ret: string;
    };

    struct {
        name: string = "";

        func greeting() string {
            return string.format("hello, {}", this.name);
        }

        func is_callable() bool {
            return switch this {
                Node.FnDef => true,
                else => false
            };
        }

        func type_name() string {
            return this.display();
        }
    }
}

func main() {
    var node: Node.FnDef = {.name = "f", .params = {}, .ret = "int"};
    printf("node.type: {}\n", node.type);
    printf("node.discriminator: {}\n", node.discriminator());
    printf("node.ret: {}\n", node.ret);
    printf("node.name: {}\n", node.name);
    printf("greeting: {}\n", node.greeting());
    printf("discriminator value: {}\n", node.discriminator());
    printf("is_callable: {}\n", node.is_callable());
    printf("type_name: {}\n", node.type_name());
}

