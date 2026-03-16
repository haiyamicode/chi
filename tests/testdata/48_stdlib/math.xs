import "std/math" as math;

func main() {
    printf("PI = {}\n", math.PI);
    printf("E = {}\n", math.E);
    printf("abs(-5.0) = {}\n", math.abs(-5.0));
    printf("sqrt(16.0) = {}\n", math.sqrt(16.0));
    printf("floor(3.7) = {}\n", math.floor(3.7));
    printf("ceil(3.2) = {}\n", math.ceil(3.2));
    printf("round(3.5) = {}\n", math.round(3.5));
    printf("min(1.0, 2.0) = {}\n", math.min(1.0, 2.0));
    printf("max(1.0, 2.0) = {}\n", math.max(1.0, 2.0));
    printf("clamp(5.0, 0.0, 3.0) = {}\n", math.clamp(5.0, 0.0, 3.0));
    printf("pow(2.0, 10.0) = {}\n", math.pow(2.0, 10.0));
    printf("sin(0.0) = {}\n", math.sin(0.0));
    printf("cos(0.0) = {}\n", math.cos(0.0));
    printf("log(E) = {}\n", math.log(math.E));
    printf("is_nan(0.0) = {}\n", math.is_nan(0.0));
    printf("is_nan(NAN) = {}\n", math.is_nan(math.NAN));
    printf("is_inf(INF) = {}\n", math.is_inf(math.INF));
    printf("mod(10.0, 3.0) = {}\n", math.mod(10.0, 3.0));
}
