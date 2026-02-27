import "std/ops" as ops;

func identity<T>(value: T) T {
    return value;
}

func transform<T, U>(value: T, transformer: func (value: T) U) U {
    return transformer(value);
}

struct Container<T: ops.Add> {
    value: T;

    func new(value: T) {
        this.value = value;
    }

    func get() T {
        return this.value;
    }

    func zmap<U>(transform: func (value: T) U) Container<U> {
        return {transform(this.value)};
    }

    func add(other: Container<T>) Container<T> {
        return {this.value + other.value};
    }
}

func main() {
    var int_result = identity<int>(42);
    printf("identity<int>(42) = {}\n", int_result);
    var int_result_inferred = identity(42);
    printf("identity(42) [inferred] = {}\n", int_result_inferred);
    var char_result = identity<char>('A');
    printf("identity<char>('A') = {}\n", char_result);
    var char_result_inferred = identity('A');
    printf("identity('A') [inferred] = {}\n", char_result_inferred);
    var doubled = transform<int, int>(5, func (x: int) int {
        return x * 2;
    });
    printf("transform<int, int>(5, double) = {}\n", doubled);
    var doubled_inferred = transform(5, func (x: int) int {
        return x * 2;
    });
    printf("transform(5, double) [inferred] = {}\n", doubled_inferred);
    var container = Container<int>{65};
    var zmap_result = container.zmap<char>(func (value: int) char {
        return (value + 10) as char;
    });
    var final_value = zmap_result.get();
    printf("final value: {}\n", final_value);
    var zmap_result_inferred = container.zmap(func (value: int) char {
        return (value + 10) as char;
    });
    var final_value_inferred = zmap_result_inferred.get();
    printf("final value [inferred]: {}\n", final_value_inferred);
    var float_container = container.zmap<float>(func (i: int) float {
        return i as float * 0.5;
    });
    printf("float result: {}\n", float_container.get());
    var float_container_inferred = container.zmap(func (i: int) float {
        return i as float * 0.5;
    });
    printf("float result [inferred]: {}\n", float_container_inferred.get());
    var c1 = Container<int>{100};
    var c2 = Container<int>{200};
    var added = c1.add(c2);
    printf("added: {}\n", added.get());
}

