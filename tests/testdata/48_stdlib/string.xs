func main() {
    // byte-level basics
    var s = "hello";
    printf("byte_length = {}\n", s.byte_length());
    printf("byte_at(0) = '{}'\n", s.byte_at(0));
    printf("byte_slice(1,4) = '{}'\n", s.byte_slice(1, 4));

    // char_length - ASCII
    printf("char_length('hello') = {}\n", s.char_length());

    // char_length - multibyte
    printf("char_length('héllo') = {}\n", "héllo".char_length());
    printf("byte_length('héllo') = {}\n", "héllo".byte_length());
    printf("char_length('你好世界') = {}\n", "你好世界".char_length());
    printf("byte_length('你好世界') = {}\n", "你好世界".byte_length());

    // at - ASCII
    printf("at(0) = '{}'\n", "abcde".at(0));
    printf("at(4) = '{}'\n", "abcde".at(4));

    // at - multibyte
    printf("at(1) of 'héllo' = '{}'\n", "héllo".at(1));
    printf("at(0) of '你好' = '{}'\n", "你好".at(0));
    printf("at(1) of '你好' = '{}'\n", "你好".at(1));

    // at - emoji (4-byte codepoint)
    var emoji = "a😀b";
    printf("char_length('a😀b') = {}\n", emoji.char_length());
    printf("byte_length('a😀b') = {}\n", emoji.byte_length());
    printf("at(0) of 'a😀b' = '{}'\n", emoji.at(0));
    printf("at(1) of 'a😀b' = '{}'\n", emoji.at(1));
    printf("at(2) of 'a😀b' = '{}'\n", emoji.at(2));

    // slice
    printf("slice(0,5) of 'hello world' = '{}'\n", "hello world".slice(0, 5));
    printf("slice(6,null) of 'hello world' = '{}'\n", "hello world".slice(6, null));
    printf("slice(0,2) of 'héllo' = '{}'\n", "héllo".slice(0, 2));
    printf("slice(2,null) of 'héllo' = '{}'\n", "héllo".slice(2, null));
    printf("slice(1,2) of '你好世界' = '{}'\n", "你好世界".slice(1, 2));
    printf("slice(0,1) of 'a😀b' = '{}'\n", emoji.slice(0, 1));
    printf("slice(1,2) of 'a😀b' = '{}'\n", emoji.slice(1, 2));
    printf("slice(2,3) of 'a😀b' = '{}'\n", emoji.slice(2, 3));

    // [..] syntax
    printf("[0..5] of 'hello world' = '{}'\n", "hello world"[0..5]);
    printf("[6..] of 'hello world' = '{}'\n", "hello world"[6..]);
    printf("[..5] of 'hello world' = '{}'\n", "hello world"[..5]);
    printf("[1..2] of 'a😀b' = '{}'\n", emoji[1..2]);
    printf("[0..2] of '你好世界' = '{}'\n", "你好世界"[0..2]);

    // contains
    printf("contains('llo') = {}\n", "hello".contains("llo"));
    printf("contains('xyz') = {}\n", "hello".contains("xyz"));
    printf("contains('') = {}\n", "hello".contains(""));
    printf("contains('好') = {}\n", "你好世界".contains("好"));

    // starts_with / ends_with
    printf("starts_with('hel') = {}\n", "hello".starts_with("hel"));
    printf("starts_with('llo') = {}\n", "hello".starts_with("llo"));
    printf("ends_with('llo') = {}\n", "hello".ends_with("llo"));
    printf("ends_with('hel') = {}\n", "hello".ends_with("hel"));

    // split
    var parts = "a,b,c".split(",");
    printf("split len = {}\n", parts.length);
    printf("split[0] = '{}'\n", parts[0]);
    printf("split[1] = '{}'\n", parts[1]);
    printf("split[2] = '{}'\n", parts[2]);

    var parts2 = "no-sep".split(",");
    printf("split nosep len = {}\n", parts2.length);
    printf("split nosep[0] = '{}'\n", parts2[0]);

    // replace_all
    printf("replace_all = '{}'\n", "foo bar foo".replace_all("foo", "baz"));
    printf("replace_all none = '{}'\n", "hello".replace_all("xyz", "abc"));

    // repeat
    printf("repeat(0) = '{}'\n", "ab".repeat(0));
    printf("repeat(3) = '{}'\n", "ab".repeat(3));

    // trim
    printf("trim = '{}'\n", "  hello  ".trim());
    printf("trim_left = '{}'\n", "  hello  ".trim_left());
    printf("trim_right = '{}'\n", "  hello  ".trim_right());
    printf("trim empty = '{}'\n", "   ".trim());

    // multiline continuation
    var continued = "helloworld";
    printf("continued = '{}'\n", continued);
}
