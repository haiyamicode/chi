enum Key {
    Enter = 1,
    Space,
    Ctrl = 10,
    Alt,
    Delete
}

func main() {
    printf("enter_key_value: {}\n", Key.Enter); 
    printf("space_key_value: {}\n", Key.Space);
    var key: Key = 12;
    printf("key_value: {}\n", key);
    if key == Key.Delete {
        printf("delele_key: {}\n", key);
    }
}