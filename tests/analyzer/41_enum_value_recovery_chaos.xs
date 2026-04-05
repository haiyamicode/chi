// Malformed enum explicit values and payloads should recover without crashing

enum Status (type: int) {
    Idle = "zero",
    Running = 1 + ,
    Paused(int,, string),
    Broken {
        code int
        label: string string
    };
    Done = (4 * );
    Final
}

func still_parses() {
    var x = 1;
    var y = x + 2;
}
