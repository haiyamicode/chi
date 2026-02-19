// Returning second ref param when return lifetime elides to first.
// b has its own lifetime, can't satisfy return's 'a lifetime.

func bigger(a: &int, b: &int) &int {
    if a! > b! {
        return a;
    }
    return b;
}

func main() {
    var x = 10;
    var y = 20;
    var r = bigger(&x, &y);
}
