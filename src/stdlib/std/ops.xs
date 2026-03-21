export interface Display {
    func display() string;
}

export interface Index<K, V> {
    func index(key: K) &V;
}

export interface IndexMut<K, V> {
    func index_mut(index: K) &mut V;
}

export interface Copy {
    func copy(source: &This);
}

export interface NoCopy {}

export interface IndexMutIterable<K, V> {
    func index_mut(index: K) &mut V;
    func begin() K;
    func end() K;
    func next(index: K) K;
}

export interface ListInit<T> {
    func list_init(...items: T);
}

export interface Add {
    func add(rhs: &This) This;
}

export interface Sized {}

export interface Unsized {}

export interface Construct {
    func new();
}

export interface MutIterator<T> {
    func next() ?(&mut T);
}

export interface MutIterable<T> {
    func to_iter_mut() MutIterator<T>;
}

export interface Unwrap<T> {
    func unwrap() &T;
}

export interface UnwrapMut<T> {
    func unwrap_mut() &mut T;
}

export interface Deref<T: Unsized + NoCopy> {
    func deref() &T;
}

export interface DerefMut<T: Unsized + NoCopy> {
    func deref_mut() &mut T;
}

export interface Slice<Out> {
    func slice(start: ?uint32, end: ?uint32) Out;
}

export interface Sub {
    func sub(rhs: &This) This;
}

export interface Mul {
    func mul(rhs: &This) This;
}

export interface Div {
    func div(rhs: &This) This;
}

export interface Rem {
    func rem(rhs: &This) This;
}

export interface Neg {
    func neg() This;
}

export interface BitAnd {
    func bitand(rhs: &This) This;
}

export interface BitOr {
    func bitor(rhs: &This) This;
}

export interface BitXor {
    func bitxor(rhs: &This) This;
}

export interface Not {
    func not() This;
}

export interface Shl {
    func shl(rhs: &This) This;
}

export interface Shr {
    func shr(rhs: &This) This;
}

export interface Eq {
    func eq(other: &This) bool;
}

export interface Ord {
    func cmp(other: &This) int;
}

export interface Hash {
    func hash() uint64;
}

export interface AsTuple<...T> {
    func as_tuple() Tuple<...T>;
}

// Composite numeric interfaces
export interface Number {
    ...Add;
    ...Sub;
    ...Mul;
    ...Div;
    ...Rem;
    ...Neg;
    ...Eq;
    ...Ord;
}

export interface Float {
    ...Number;
}

export interface Int {
    ...Number;
    ...BitAnd;
    ...BitOr;
    ...BitXor;
    ...Not;
    ...Shl;
    ...Shr;
}
