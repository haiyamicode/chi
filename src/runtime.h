#pragma once
namespace cx {
namespace runtime {

static const char *source = R""""(
extern "C" {
  func cx_print(str string);
  func cx_printf(format string, values array<any>);
  func cx_array_construct(dest *void);
  func cx_print_any(value any);
  func cx_print_number(value int);
  func cx_gc_alloc(size uint32, destructor *void) *void;
  func cx_runtime_start(stack *void);
  func cx_runtime_stop();
}

export func println(value any) {
  cx_print_any(value);
  cx_print("\n");
}

export func gc_alloc(size uint32) *void {
  return cx_gc_alloc(size, 0);
}

export func print_int(value int64) {
  cx_print_number(value);
}

// export func printf(format string, ...values any) {
//   cx_printf(format, values);
// }

)"""";

} // namespace runtime

} // namespace cx