// Test struct implements without opening brace - causes infinite loop
interface Animal {
    func speak();
}

struct Dog implements Animal
    name: string;