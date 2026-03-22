import {Boxed, Holder} from "./testdata/57_managed_lowlevel_custom_ctor_array_escape_holder";

func collect() Holder {
    var h = Holder{};
    for i in 0..3 {
        var obj = Boxed{id: 1500 + i};
        h.inner.refs.push(&obj);
    }
    return h;
}

func main() {
    var h = collect();
    printf("refs = [{}, {}, {}]\n", h.inner.refs[0].id, h.inner.refs[1].id, h.inner.refs[2].id);
}
