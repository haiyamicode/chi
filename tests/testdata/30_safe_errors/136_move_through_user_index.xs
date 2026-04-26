// expect-error: cannot move out of a non-owning reference
import "std/ops" as ops;

struct MyArray {
    b: Box<int>;

    impl ops.Index<int, Box<int>> {
        func index(key: int) &Box<int> { return &this.b; }
    }
}

func take(b: Box<int>) {}

func main() {
    var ma = MyArray{b: Box<int>.from_value(1)};
    take(move ma[0]);
}
