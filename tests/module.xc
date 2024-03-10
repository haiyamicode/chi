struct Greeting {
  name string;

  func new(name string) {
    this.name = name;
  }

  func hello() {
    printf("hello, {}\n", this.name);
  }
}

func hello() {
  println("hello from another module");
}