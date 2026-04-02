import "test_registry/mylib" as mylib;
import {Greeter} from "test_registry/mylib";

func main() {
    println("package import test");
    mylib.default_greeting();
    mylib.count_to(3);
    var g = mylib.Greeter{"world"};
    g.greet();
    var g2 = Greeter{"named import"};
    g2.greet();
}
