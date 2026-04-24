// Reject implicit &move T -> &I widening (and related ref-kind mismatches).
// Dropping the move marker while widening to an interface ref silently
// creates a borrow, so the conversion must require an
// explicit cast.

interface Shape {}

struct Circle {
    r: int = 0;
    impl Shape {}
}

// Error: &move Circle -> &Shape (drops ownership)
func bad_widen_borrow() &Shape {
    return new Circle{r: 1};
}

// Error: &move Circle -> &mut Shape (drops ownership)
func bad_widen_mut() &mut Shape {
    return new Circle{r: 2};
}

// Error: &Circle -> &move Shape (can't upgrade borrow to owner)
func bad_narrow_to_move(c: &Circle) &move Shape {
    return c;
}

// Error: &Circle -> &mut Shape (can't widen ref to mutable)
func bad_ref_to_mut(c: &Circle) &mut Shape {
    return c;
}

func main() {}
