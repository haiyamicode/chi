interface Animal {
    func make_sound();}

struct AnimalBase implements Animal {
    id: int;

    mut func new(id: int) {
        this.id = id;
    }

    func make_sound() {
        printf("{}: <silence>\n", this.id);
    }
}

struct Ant {
    ...base: AnimalBase;
}

struct Sheep {
    ...base: AnimalBase;

    mut func new(id: int) {
        this.base = {id};
    }

    func make_sound() {
        printf("{}: baaaaahh\n", this.id);
    }
}

struct Cat implements Animal {
    id: int;

    mut func new(id: int) {
        this.id = id;
    }

    func make_sound() {
        printf("{}: meeoooww\n", this.id);
    }
}

func main() {
    var sheep: Sheep = {1};
    var cat: Cat = {2};
    var ant: Ant = {3};
    var animal: Animal = &sheep;
    animal.make_sound();
    animal = &cat;
    animal.make_sound();
    animal = &ant;
    animal.make_sound();
}

