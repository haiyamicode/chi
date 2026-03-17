import "std/fs" as fs;
import "std/time" as time;

extern "C" {
    unsafe func cx_thread_spawn(callback: *void);
    func cx_is_main_thread() bool;
}

let TEST_PATH = "/tmp/chi_stdlib_thread_uv_test.txt";

func main() {
    if fs.exists(TEST_PATH) {
        fs.remove(TEST_PATH);
    }
    fs.write_file(TEST_PATH, "hello");

    printf("main = {}\n", cx_is_main_thread());

    for i in 0..10 {
        let worker = func [i] () {
            let text = fs.read_file(TEST_PATH);
            assert(text == "hello");
            let worker_is_main = cx_is_main_thread();
            assert(!worker_is_main);
            let delay = ((10 - i) * 20) as uint64;
            time.timeout(
                delay,
                func [i, text, worker_is_main] () {
                    printf(
                        "worker {} main={} text={} timeout={}\n",
                        i,
                        worker_is_main,
                        text,
                        cx_is_main_thread()
                    );
                }
            );
        };

        unsafe {
            cx_thread_spawn(&worker);
        }
    }

    time.timeout(
        250,
        func () {
            printf("cleanup = {}\n", cx_is_main_thread());
            if fs.exists(TEST_PATH) {
                fs.remove(TEST_PATH);
            }
        }
    );

    println("done");
}
