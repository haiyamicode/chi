enum Key {
    Enter = 1,
    Space,
    Ctrl = 10,
    Alt,
    Delete
  };
  
  func main() {
    printf("enter_key_value: {}\n", Key.Enter.value); 
    printf("space_key_value: {}\n", Key.Space.value);
    var key: Key = Key.Delete;
    printf("key_value: {}\n", key.value);
    if key == Key.Delete {
      printf("delele_key: {}\n", key);
    }
  }
  