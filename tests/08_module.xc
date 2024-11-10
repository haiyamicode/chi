import "./module" as mod;
import "./module" { Greeting, hello as test };

func main() {
  println("using function from module");
  mod.hello();
  
  println("using type from module");
  var g mod.Greeting = {"Lila"};
  g.hello();

  println("using imported members from module");
  test();
  var g2 Greeting = {"Xenia"};
  g2.hello();
}