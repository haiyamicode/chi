import "std/ops" as ops;

interface Cloneable {
    func clone() This;
}

struct Color implements ops.Display, Cloneable {
    private r: int = 0;
    private g: int = 0;
    protected b: int = 0;

    func new(r: int, g: int, b: int) {
        this.r = r;
        this.g = g;
        this.b = b;
    }

    static func black() This {
        return {0, 0, 0};
    }

    static func white() This {
        return {255, 255, 255};
    }

    static func gray(f: float) This {
        return Color.white().multiply(f);
    }

    static func red() This {
        return {255, 0, 0};
    }

    static func create_copy(other: This) This {
        return {other.r, other.g, other.b};
    }

    func multiply(f: float) This {
        let rf = this.r as float;
        let gf = this.g as float;
        let bf = this.b as float;
        let r = (rf * f) as int;
        let g = (gf * f) as int;
        let b = (bf * f) as int;
        return {r, g, b};
    }

    func display() string {
        return string.format("rgb({},{},{})", this.r, this.g, this.b);
    }

    func brightness() float {
        let t = this.r + this.g + this.b;
        return (t as float) / 3.0 / 255.0;
    }

    func clone() This {
        return {this.r, this.g, this.b};
    }
}

func main() {
    let a = Color.black();
    printf("a: {}\n", a);
    let b = Color.gray(0.6);
    printf("b: {}\n", b);
    let c = Color.white();
    printf("c: {}\n", c);
    let d = Color.red();
    printf("d: {}\n", d);
    printf("a.brightness = {}\n", a.brightness());
    printf("b.brightness = {}\n", b.brightness());
    printf("c.brightness = {}\n", c.brightness());
    printf("d.brightness = {}\n", d.brightness());
    let e = Color.create_copy(d);
    printf("e (copy of d): {}\n", e);
    let cloned_color = c.clone();
    printf("cloned white: {}\n", cloned_color);
}

