import "std/pkg" as pkg;

func main() {
    let info = pkg.info();
    printf("name={}\n", info.name ?? "<null>");
    printf("version={}\n", info.version ?? "<null>");
    printf("description={}\n", info.description ?? "<null>");
}
