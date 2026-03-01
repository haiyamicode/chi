interface Display {
    func display() string;
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

// interface Sub<T> {
//   func sub(rhs: T) T;
// }
// interface Mul<T> {
//   func mul(rhs: T) T;
// }
// interface Div<T> {
//   func div(rhs: T) T;
// }
// interface Rem<T> {
//   func rem(rhs: T) T;
// }
// interface Neg<T> {
//   func neg() T;
// }
// interface AddAssign<Rhs> {
//   mut func add_assign(rhs: Rhs);
// }
// interface SubAssign<Rhs> {
//   mut func sub_assign(rhs: Rhs);
// }
// interface MulAssign<Rhs> {
//   mut func mul_assign(rhs: Rhs);
// }
// interface DivAssign<Rhs> {
//   mut func div_assign(rhs: Rhs);
// }
// interface RemAssign<Rhs> {
//   mut func rem_assign(rhs: Rhs);
// }
// interface BitAnd<T> {
//   func bitand(rhs: T) T;
// }
// interface BitOr<T> {
//   func bitor(rhs: T) T;
// }
// interface BitXor<T> {
//   func bitxor(rhs: T) T;
// }
// interface Not<T> {
//   func not() T;
// }
// interface Shl<T, Rhs> {
//   func shl(rhs: Rhs) T;
// }
// interface Shr<T, Rhs> {
//   func shr(rhs: Rhs) T;
// }
// interface BitAndAssign<Rhs> {
//   mut func bitand_assign(rhs: Rhs);
// }
// interface BitOrAssign<Rhs> {
//   mut func bitor_assign(rhs: Rhs);
// }
// interface BitXorAssign<Rhs> {
//   mut func bitxor_assign(rhs: Rhs);
// }
// interface ShlAssign<Rhs> {
//   mut func shl_assign(rhs: Rhs);
// }
// interface ShrAssign<Rhs> {
//   mut func shr_assign(rhs: Rhs);
// }
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
