interface Display {
  func display() string;
}

interface Index<K, V> {
  func index(index: K) &mut<V>;
}

interface CopyFrom<T> {
  func copy_from(from: &T);
}

interface IndexIterable<K, V> {
  func index(index: K) &mut<V>;
  func begin() K;
  func end() K;
  func next(index: K) K;
}