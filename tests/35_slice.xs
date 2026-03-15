func main() {
    var arr: Array<int> = [1, 2, 3, 4, 5];

    // Full range slice
    var s1 = arr[1..3];
    printf("s1: {}\n", s1);

    // Open end
    var s2 = arr[2..];
    printf("s2: {}\n", s2);

    // Open start
    var s3 = arr[..3];
    printf("s3: {}\n", s3);

    // Full open
    var s4 = arr[..];
    printf("s4: {}\n", s4);

    // Empty slice
    var s5 = arr[2..2];
    printf("s5: {}\n", s5);

    // Single element
    var s6 = arr[0..1];
    printf("s6: {}\n", s6);
}
