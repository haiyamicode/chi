func for_classic() {
    println("for_classic:");
    printf("print from 1 to 5:\n", 0);

    for i in 0..5 {
        printf("{}\n", i + 1);
    }

    println("");
}

func for_in() {
    println("for_in:");
    var list: Array<int> = [];
    list.push(1);
    list.push(2);
    list.push(3);
    println("print from 1 to 3");

    for &item in list {
        printf("{}\n", item);
    }

    println("");
    println("modify the list and print again");

    for &mut item in list {
        *item = *item + 1;
    }

    for item in list {
        printf("{}\n", item);
    }

    println("");
}

func for_in_indexed() {
    println("for_in_indexed:");
    var list: Array<int> = [10, 20, 30];

    for item, i in list {
        printf("{}: {}\n", i, item);
    }

    println("");
}

func empty_while() {
    println("empty_while:");
    printf("print from 1 to 5 without 4:\n", 0);
    var i: int = 0;

    while {
        if i == 3 {
            i++;
            continue;
        } else if i >= 5 {
            break;
        }

        printf("{}\n", i + 1);
        i++;
    }

    println("");
}

func while_with_condition() {
    println("while_with_condition:");
    printf("print from 1 to 5:\n", 0);
    var i: int = 1;

    while i <= 5 {
        printf("{}\n", i);
        i++;
    }

    println("");
}

func main() {
    for_classic();
    for_in();
    for_in_indexed();
    empty_while();
    while_with_condition();
}
