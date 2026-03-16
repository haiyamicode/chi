import "std/conv" as conv;

func main() {
    printf("parse_int('42') = {}\n", conv.parse_int("42"));
    printf("parse_int('-100') = {}\n", conv.parse_int("-100"));
    printf("parse_int('abc') = {}\n", conv.parse_int("abc"));
    printf("parse_int('') = {}\n", conv.parse_int(""));
    printf("parse_float('3.14') = {}\n", conv.parse_float("3.14"));
    printf("parse_float('-0.5') = {}\n", conv.parse_float("-0.5"));
    printf("parse_float('abc') = {}\n", conv.parse_float("abc"));
    printf("parse_bool('true') = {}\n", conv.parse_bool("true"));
    printf("parse_bool('false') = {}\n", conv.parse_bool("false"));
    printf("parse_bool('1') = {}\n", conv.parse_bool("1"));
    printf("parse_bool('0') = {}\n", conv.parse_bool("0"));
    printf("parse_bool('maybe') = {}\n", conv.parse_bool("maybe"));
}
