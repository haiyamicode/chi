func test_integer_bases() {
    println("=== integer bases ===");
    printf("{:x}\n", 0);
    printf("{:x}\n", 255);
    printf("{:x}\n", 65535);
    printf("{:X}\n", 255);
    printf("{:X}\n", 48879);
    printf("{:b}\n", 0);
    printf("{:b}\n", 1);
    printf("{:b}\n", 42);
    printf("{:b}\n", 255);
    printf("{:o}\n", 0);
    printf("{:o}\n", 8);
    printf("{:o}\n", 255);
    printf("{:o}\n", 511);
}

func test_alt_form() {
    println("=== alt form ===");
    printf("{:#x}\n", 0);
    printf("{:#x}\n", 255);
    printf("{:#X}\n", 255);
    printf("{:#b}\n", 0);
    printf("{:#b}\n", 42);
    printf("{:#o}\n", 0);
    printf("{:#o}\n", 255);
}

func test_float_precision() {
    println("=== float precision ===");
    printf("{:.0}\n", 3.14159);
    printf("{:.1}\n", 3.14159);
    printf("{:.2}\n", 3.14159);
    printf("{:.3}\n", 3.14159);
    printf("{:.5}\n", 3.14159);
    printf("{:.2}\n", 0.0);
    printf("{:.2}\n", -1.5);
    printf("{:.4}\n", 100.0);
}

func test_scientific() {
    println("=== scientific ===");
    printf("{:e}\n", 0.0);
    printf("{:e}\n", 12345.6789);
    printf("{:E}\n", 12345.6789);
    printf("{:.2e}\n", 12345.6789);
    printf("{:.0e}\n", 12345.6789);
    printf("{:e}\n", -42.5);
    printf("{:.3e}\n", 0.001234);
}

func test_width_align() {
    println("=== width and alignment ===");
    // default: numbers right, strings left
    printf("[{:10}]\n", 42);
    printf("[{:10}]\n", "hi");
    // explicit
    printf("[{:<10}]\n", 42);
    printf("[{:>10}]\n", 42);
    printf("[{:^10}]\n", 42);
    printf("[{:<10}]\n", "hi");
    printf("[{:>10}]\n", "hi");
    printf("[{:^10}]\n", "hi");
    // odd centering
    printf("[{:^9}]\n", "hi");
    printf("[{:^11}]\n", "abc");
    // width smaller than content (no truncation)
    printf("[{:2}]\n", "hello");
    printf("[{:1}]\n", 12345);
}

func test_fill_char() {
    println("=== fill character ===");
    printf("[{:.<10}]\n", "hi");
    printf("[{:.>10}]\n", "hi");
    printf("[{:.^10}]\n", "hi");
    printf("[{:_<10}]\n", 42);
    printf("[{:_>10}]\n", 42);
    printf("[{:_^10}]\n", 42);
    printf("[{:-<10}]\n", "x");
    printf("[{:*>10}]\n", "x");
}

func test_zero_pad() {
    println("=== zero padding ===");
    printf("[{:05}]\n", 42);
    printf("[{:05}]\n", -42);
    printf("[{:010}]\n", 255);
    printf("[{:08x}]\n", 255);
    printf("[{:08X}]\n", 255);
    printf("[{:08b}]\n", 42);
    printf("[{:08o}]\n", 255);
    // zero pad with alt form
    printf("[{:#010x}]\n", 255);
    printf("[{:#010b}]\n", 42);
    printf("[{:#010o}]\n", 255);
}

func test_sign() {
    println("=== sign ===");
    printf("[{:+}]\n", 0);
    printf("[{:+}]\n", 42);
    printf("[{:+}]\n", -42);
    printf("[{:+.2}]\n", 3.14);
    printf("[{:+.2}]\n", -3.14);
    printf("[{:+.2}]\n", 0.0);
}

func test_combinations() {
    println("=== combinations ===");
    // sign + width
    printf("[{:+10}]\n", 42);
    printf("[{:+10}]\n", -42);
    // sign + zero pad
    printf("[{:+010}]\n", 42);
    printf("[{:+010}]\n", -42);
    // alt form + zero pad + width
    printf("[{:#010x}]\n", 4095);
    printf("[{:#016b}]\n", 255);
    // float precision + width
    printf("[{:10.2}]\n", 3.14159);
    printf("[{:<10.2}]\n", 3.14159);
    // scientific + width
    printf("[{:15e}]\n", 12345.6789);
    printf("[{:15.2e}]\n", 12345.6789);
}

func test_different_int_types() {
    println("=== int types ===");
    var u8: uint8 = 200 as uint8;
    var u16: uint16 = 50000 as uint16;
    var u32: uint32 = 3000000000 as uint32;
    var i8: int8 = -100 as int8;
    var i16: int16 = -30000 as int16;
    printf("{:x}\n", u8);
    printf("{:x}\n", u16);
    printf("{:x}\n", u32);
    printf("{}\n", i8);
    printf("{}\n", i16);
    printf("{:+}\n", i8);
}

func test_escapes() {
    println("=== escapes ===");
    printf("{{}}\n");
    printf("{{hello}}\n");
    printf("{} {{}}\n", "a");
    printf("{{}} {}\n", "b");
}

func test_stringf() {
    println("=== stringf ===");
    var s1 = stringf("{:x}", 255);
    var s2 = stringf("{:#010x}", 4095);
    var s3 = stringf("{:.2}", 3.14159);
    var s4 = stringf("[{:>10}]", "hello");
    println(s1);
    println(s2);
    println(s3);
    println(s4);
}

func main() {
    test_integer_bases();
    test_alt_form();
    test_float_precision();
    test_scientific();
    test_width_align();
    test_fill_char();
    test_zero_pad();
    test_sign();
    test_combinations();
    test_different_int_types();
    test_escapes();
    test_stringf();
}
