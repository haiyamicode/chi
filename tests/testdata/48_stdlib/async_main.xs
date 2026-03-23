import "std/time" as time;

async func main() Promise {
    println("=== async main ===");
    await time.sleep(1);
    println("after await");
}
