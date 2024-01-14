#pragma once
namespace cx {
namespace runtime {

static const char *source = R""""(
extern "C" {
  func cx_print(str string);
  func cx_gc_alloc(size uint32, destructor *void) *void;
}

export func println(str string) {
  cx_print(str);
  cx_print("\n");
}

)"""";

} // namespace runtime

} // namespace cx