// Chaotic async/generic/lambda soup should report errors, not crash
import "std/runtime"

async func broken<T>(p: Promise<T>) Promise<int> {
    var value = await p.;
    return p.settle(func (result {
        switch result {
            Ok(value) => value,
            Err{error} => throw error,
        }
    });
}

func main() {
    var p = Promise<int>{;
    p.then(func (x) => );
    var q = broken<int(>(p);
}
