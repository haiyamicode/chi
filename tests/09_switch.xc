func switch_int() {
  var levels Array<int> = {};
  levels.add(1);
  levels.add(2);
  levels.add(3);
  levels.add(4);

  for levels: item {
    var label = switch item! {
      1, 2 => "low",
      3 => "medium",
      else => {
        println("default case:");
        "high"
      }
    };
    println(label);
  }
}

func main() {
  switch_int();
}