// Generic temp holder should not hide ref escape through copy edges.
// expect-error: does not live long enough

struct Thing {
    id: int = 0;
}

struct GenericTempHolder<T> {
    value: T;

    mut func set(value: T) {
        let tmp = value;
        this.value = tmp;
    }

    func get() T {
        let tmp = this.value;
        return tmp;
    }
}

func make_holder() GenericTempHolder<&Thing> {
    var obj = Thing{id: 800};
    var holder = GenericTempHolder<&Thing>{};
    holder.set(&obj);
    return holder;
}

func main() {
    let holder = make_holder();
    println(holder.get().id);
}
