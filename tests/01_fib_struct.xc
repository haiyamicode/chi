func main() {
    var fib: Fib = {12, .name = "test"};
    printf("fib {}: {}\n", fib.name, fib.calculate());
}

struct Fib {
    n: int;
    name: string = "";

    mut func new(n: int) {
        this.n = n;
    }

    func calculate() int {
        return this.fib(this.n);
    }

    func fib(n: int) int {
        if n < 2 {
            return n;
        }
        return this.fib(n - 2) + this.fib(n - 1);
    }
}

