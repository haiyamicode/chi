import {Boxed, Holder} from "./testdata/58_managed_lowlevel_custom_ctor_map_escape_holder";

func collect() Holder {
    var h = Holder{};
    for i in 0..3 {
        var obj = Boxed{id: 1600 + i};
        h.inner.refs.set(i, &obj);
    }
    return h;
}

func main() {
    var h = collect();
    printf("refs = [{}, {}, {}]\n",
           h.inner.refs[0].id,
           h.inner.refs[1].id,
           h.inner.refs[2].id);
}
