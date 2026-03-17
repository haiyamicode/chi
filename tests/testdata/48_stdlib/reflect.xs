import "std/reflect" as reflect;

interface Shape {}

struct Point {
    x: int = 0;
    y: int = 0;

    impl Shape {}
}

func main() {
    let point = Point{x: 3, y: 7};
    let point_type = point.(type);
    printf("static: {}\n", point_type);
    printf("static fields: {}\n", point_type.fields());

    let shape: &Shape = &point;
    let shape_type = shape.(type);
    printf("shape type: {}\n", shape_type);

    let elem = shape_type.elem();
    if elem {
        printf("shape elem: {}\n", elem);
    }

    let dyn_elem = shape_type.dyn_elem(shape);
    if dyn_elem {
        printf("shape dyn elem: {}\n", dyn_elem);
        printf("shape dyn fields: {}\n", dyn_elem.fields());
    }

    switch shape.(type) {
        &Point => println("switch ok"),
        else => println("switch bad")
    }
}
