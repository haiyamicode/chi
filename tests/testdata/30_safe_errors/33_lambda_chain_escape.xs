// Chain: local -> lambda A captures it -> lambda B captures A -> B escapes.
// The transitive capture chain must be detected.
// expect-error: does not live long enough

func make_chain() (func () int) {
    var secret = 777;
    var step1 = func () int {
        return secret;
    };
    var step2 = func () int {
        return step1() + 1;
    };
    return step2;
}

func main() {
    printf("{}\n", make_chain()());
}

