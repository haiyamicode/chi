#pragma once
namespace cx {
namespace runtime {

const char *source = R""""(
extern "C" {
  func cx_print(str string);
}

export func println(str string) {
  cx_print(str);
  cx_print("\n");
}

)"""";

} // namespace runtime

} // namespace cx