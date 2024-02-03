#pragma once
namespace cx {
namespace runtime {

static const char *source = R""""(
extern "C" {
  func cx_print(str string);
  // func cx_printf(format string, values Array<any>);
  func cx_array_construct(dest *void);
  func cx_print_any(value *void);
  func cx_print_number(value uint64);
  func cx_gc_alloc(size uint32) *void;
  func cx_runtime_start(stack *void);
  func cx_runtime_stop();
  func cx_panic(message string);
  func cx_personality(...) int32;
  func cx_timeout(delay uint64, callback *void);
  func cx_call(fn *void);
}

export func println(value any) {
  cx_print_any(&value);
  cx_print("\n");
}

export func gc_alloc(size uint32) *void {
  return cx_gc_alloc(size);
}

export func print_int(value uint64) {
  cx_print_number(value);
}

// export func printf(format string, ...values any) {
//   cx_printf(format, values);
// }

export func panic(message string) {
  cx_panic(message);
}

export func timeout(delay uint64, callback func) {
   cx_timeout(delay, callback.ptr);
}

)"""";

} // namespace runtime

} // namespace cx