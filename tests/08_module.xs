import "./testdata/08_module" as mod;

import {Greeting, hello as test, hello_sub as test2} from "./testdata/08_module";

func main() {
    println("using function from module");
    mod.hello();
    println("using type from module");
    var g = mod.Greeting{"Lila"};
    g.hello();
    println("using imported members from module");
    test();
    var g2 = Greeting{"Xenia"};
    g2.hello();
    println("using imported members from sub-module");
    test2();
}

