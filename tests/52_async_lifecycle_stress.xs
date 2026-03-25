import "std/ops" as ops;
import "std/time" as time;

struct TraceStressValue {
    value: int = 0;

    mut func delete() {
        if this.value != 0 {
            printf("TraceStressValue.delete({})\n", this.value);
        }
    }

    impl ops.Copy {
        mut func copy(source: &This) {
            this.value = source.value;
            if source.value != 0 {
                printf("TraceStressValue.copy({})\n", source.value);
            }
        }
    }
}

struct TraceStressError {
    code: int;

    func message() string {
        return stringf("stress {}", this.code);
    }

    impl Error {}
}

async func delayed_number(value: int) Promise<int> {
    var y = await time.sleep(1);
    return value;
}

async func delayed_flag(value: bool, ms: int) Promise<bool> {
    var y = await time.sleep(ms);
    return value;
}

async func double_it(value: int) Promise<int> {
    var y = await time.sleep(1);
    return value * 2;
}

async func trace_value_after_delay(value: int) Promise<TraceStressValue> {
    var y = await time.sleep(1);
    return {:value};
}

async func trace_throw_after_delay() Promise<int> {
    var y = await time.sleep(1);
    throw new TraceStressError{code: 30};
    return 0;
}

async func complex_stress() Promise<int> {
    var total = 0;

    var first = await trace_value_after_delay(10);
    printf("TraceStress.value({})\n", first.value);
    total = total + switch await double_it(0) {
        0 => first.value,
        else => -2000
    };

    var i = 0;
    while i < 2 {
        var value = await trace_value_after_delay(11 + i);
        printf("TraceStress.value({})\n", value.value);
        total = total + try await trace_throw_after_delay() catch {
            printf("TraceStress.caught(30)\n");
            value.value + 30
        };
        i = i + 1;
    }

    for j in 0..3 {
        var step = await delayed_number(j + 1);
        if j == 0 {
            continue;
        }
        total = total + step;
        if j == 1 {
            break;
        }
    }

    if await delayed_flag(false, 20) {
        return total + 1000;
    } else if await delayed_flag(true, 20) {
        return total + 100;
    } else {
        return total + 1;
    }
}

func main() {
    complex_stress().then(func (value: int) {
        printf("stress result={}\n", value);
    });
}
