import {Boxed, Holder} from "./testdata/56_managed_lowlevel_wrapper_array_escape_holder";

func collect() Holder {
    var h = Holder{};
    for i in 0..3 {
        var obj = Boxed{id: 1100 + i};
        h.refs.push(&obj);
    }
    return h;
}

func main() {
    var h = collect();
    printf("refs = [{}, {}, {}]\n", h.refs[0].id, h.refs[1].id, h.refs[2].id);
}
