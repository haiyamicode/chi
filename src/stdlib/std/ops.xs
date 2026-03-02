interface Display {
    func display() string;
}

interface Index<K, V> {
    func index(key: K) &V;
}

interface IndexMut<K, V> {
    func index_mut(index: K) &mut V;
}

interface CopyFrom<T> {
    func copy_from(source: &T);
}

interface IndexMutIterable<K, V> {
    func index_mut(index: K) &mut V;
    func begin() K;
    func end() K;
    func next(index: K) K;
}

interface Add {
    func add(rhs: This) This;
}

interface Sized {}

interface AllowUnsized {}

interface Construct {
    func new();
}

interface MutIterator<T> {
    func next() ?(&mut T);
}

interface MutIterable<T> {
    func to_iter_mut() MutIterator<T>;
}

interface Unwrap<T> {
    func unwrap() &T;
}

interface UnwrapMut<T> {
    mut func unwrap_mut() &mut T;
}

interface Slice<Out> {
    func slice(start: ?uint32, end: ?uint32) Out;
}

interface Sub {
    func sub(rhs: This) This;
}

interface Mul {
    func mul(rhs: This) This;
}

interface Div {
    func div(rhs: This) This;
}

interface Rem {
    func rem(rhs: This) This;
}

interface Neg {
    func neg() This;
}

interface BitAnd {
    func bitand(rhs: This) This;
}

interface BitOr {
    func bitor(rhs: This) This;
}

interface BitXor {
    func bitxor(rhs: This) This;
}

interface Not {
    func not() This;
}

interface Shl {
    func shl(rhs: This) This;
}

interface Shr {
    func shr(rhs: This) This;
}

interface Eq {
    func eq(other: This) bool;
}

interface Ord {
    func cmp(other: This) int;
}

// // Integer trait combining all common integer operations
// interface Int<T=This> {
//   // Inherit arithmetic operations
//   ...Add<T>;
//   ...Sub<T>;
//   ...Mul<T>;
//   ...Div<T>;
//   ...Rem<T>;
//   ...Neg<T>;
//   // Inherit arithmetic assignment operations
//   ...AddAssign<T>;
//   ...SubAssign<T>;
//   ...MulAssign<T>;
//   ...DivAssign<T>;
//   ...RemAssign<T>;
//   // Inherit bitwise operations
//   ...BitAnd<T>;
//   ...BitOr<T>;
//   ...BitXor<T>;
//   ...Not<T>;
//   ...Shl<T, T>;
//   ...Shr<T, T>;
//   // Inherit bitwise assignment operations
//   ...BitAndAssign<T>;
//   ...BitOrAssign<T>;
//   ...BitXorAssign<T>;
//   ...ShlAssign<T>;
//   ...ShrAssign<T>;
//   // Inherit comparison operations
//   ...Eq<T>;
//   ...Ord<T>;
// }
