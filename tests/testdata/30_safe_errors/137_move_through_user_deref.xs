// expect-error: cannot move out of a non-owning reference
import "std/ops" as ops;

struct MyBox {
    b: Box<int>;

    impl ops.Deref<Box<int>> {
        func deref() &Box<int> { return &this.b; }
    }
}

func take(b: Box<int>) {}

func main() {
    var mb = MyBox{b: Box<int>.from_value(1)};
    take(move *mb);
}
