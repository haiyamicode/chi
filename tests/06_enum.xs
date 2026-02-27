enum Key {
    Enter = 1,
    Space,
    Ctrl = 10,
    Alt,
    Delete
}

func main() {
    printf("enter_key_value: {}\n", Key.Enter.discriminator());
    printf("space_key_value: {}\n", Key.Space.discriminator());
    var key: Key = Key.Delete;
    printf("key_value: {}\n", key.discriminator());
    if key == Key.Delete {
        printf("delele_key: {}\n", key);
    }
}

