func for_classic() {
    println("for_classic:");
    printf("print from 1 to 5:\n", 0);
    for var i=0; i<5; i++ {
        printf("{}\n", i+1);
    }
    println("");
}

func for_in() {
    println("for_in:");
    var list Array<int> = {};
    list.add(1);
    list.add(2);
    list.add(3);

    println("print from 1 to 3");
    for &list => item {
        printf("{}\n", item!);
        item! = item! + 1;
    }

    println("print from 2 to 4");
    for list => item {
        printf("{}\n", item);
    }
    println("");
}

func empty_while() {
    println("empty_while:");
    printf("print from 1 to 5 without 4:\n", 0);
    var i int = 0;
    while {
        if i == 3 {
            i++;
            continue;
        } else if i >= 5 {
             break;
        }

        printf("{}\n", i+1);
        i++;
    }
    println("");
}

func while_with_condition() {
    println("while_with_condition:");
    printf("print from 1 to 5:\n", 0);
    var i int = 1;
    while i <= 5 {
        printf("{}\n", i);
        i++;
    }
    println("");
}

func main() {
    for_classic();
    for_in();
    empty_while();
    while_with_condition();
}