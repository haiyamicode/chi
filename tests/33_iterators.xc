import "std/ops" as ops;

// --- Linked list data structure ---

struct Node<T> {
    value: T = T{};
    next: ?*Node<T> = null;
}

struct LinkedListIterator<T> {
    current: ?*Node<T> = null;

    impl ops.MutIterator<T> {
        func next() ?(&mut T) {
            if !this.current {
                return null;
            }
            var ptr = this.current!;
            this.current = ptr.next;
            unsafe {
                return &mut ptr.value;
            }
        }
    }
}

struct LinkedList<T> {
    head: ?*Node<T> = null;

    impl ops.MutIterable<T> {
        func to_iter_mut() LinkedListIterator<T> {
            return {current: this.head};
        }
    }
}

// --- Tests ---

func test_basic_iteration() {
    println("test_basic_iteration");
    var n3 = new Node<int>{value: 3};
    var n2 = new Node<int>{value: 2, next: n3};
    var n1 = new Node<int>{value: 1, next: n2};
    var list = LinkedList<int>{head: n1};

    for item in list {
        printf("{} ", *item);
    }
    println("");

    delete n1;
    delete n2;
    delete n3;
}

func test_mutation() {
    println("test_mutation");
    var n3 = new Node<int>{value: 3};
    var n2 = new Node<int>{value: 2, next: n3};
    var n1 = new Node<int>{value: 1, next: n2};
    var list = LinkedList<int>{head: n1};

    // Mutate through iterator
    for item in list {
        *item = *item * 10;
    }

    for item in list {
        printf("{} ", *item);
    }
    println("");

    delete n1;
    delete n2;
    delete n3;
}

func test_empty_list() {
    println("test_empty_list");
    var list = LinkedList<int>{};
    var count = 0;
    for item in list {
        count = count + 1;
    }
    printf("count: {}\n", count);
}

func test_single_element() {
    println("test_single_element");
    var n1 = new Node<string>{value: "hello"};
    var list = LinkedList<string>{head: n1};

    for item in list {
        printf("{}\n", *item);
    }

    delete n1;
}

func test_manual_iterator() {
    println("test_manual_iterator");
    var n2 = new Node<int>{value: 20};
    var n1 = new Node<int>{value: 10, next: n2};
    var list = LinkedList<int>{head: n1};

    var iter = list.to_iter_mut();
    var a = iter.next();
    if a {
        printf("first: {}\n", *a);
    }
    var b = iter.next();
    if b {
        printf("second: {}\n", *b);
    }
    var c = iter.next();
    if !c {
        println("done");
    }

    delete n1;
    delete n2;
}

func main() {
    test_basic_iteration();
    test_mutation();
    test_empty_list();
    test_single_element();
    test_manual_iterator();
}

