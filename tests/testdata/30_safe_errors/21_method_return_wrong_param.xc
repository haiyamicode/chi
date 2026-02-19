// Method returning a non-this param ref.
// Return type elides to 'This, but other has its own param lifetime.

struct Holder {
    ref: &int = null;

    func bad_return(other: &int) &int {
        return other;
    }
}

func main() {
    var val = 10;
    var h = Holder{ref: &val};
    var r = h.bad_return(&val);
}
