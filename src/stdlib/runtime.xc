extern "C" {
  func cx_print(str string);
  func cx_printf(format string, values &Array<any>);
  func cx_array_new(dest *void);
  func cx_array_add(dest *void, size uint32) *void;
  func cx_print_any(value *void);
  func cx_print_number(value uint64);
  func cx_gc_alloc(size uint32, destructor *void) *void;
  func cx_malloc(size uint32, ignored *void) *void;
  func cx_free(address *void);
  func cx_runtime_start(stack *void);
  func cx_runtime_stop();
  func cx_panic(message string);
  func cx_personality(...) int32;
  func cx_timeout(delay uint64, callback *void);
  func cx_call(fn *void);
  func cx_string_format(format string, values &Array<any>) string;
}

func println(value any) {
  cx_print_any(&value);
  cx_print("\n");
}

func gc_alloc(size uint32) *void {
  return cx_gc_alloc(size, null);
}

func print_int(value uint64) {
  cx_print_number(value);
}

func printf(format string, ...values any) {
  cx_printf(format, &values);
}

func panic(message string) {
  cx_panic(message);
}

func timeout(delay uint64, callback func) {
  cx_timeout(delay, &callback);
}

func stringf(format string, ...values any) string {
  return cx_string_format(format, &values);
}

func assert(cond bool, message string) {
  if !cond {
    panic(stringf("assertion failed: {}", message));
  }
}