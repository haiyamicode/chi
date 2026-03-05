import "std/os" as os;

func main() {
    // set and get env
    os.set_env("CHI_TEST_VAR", "hello_chi");
    printf("env(CHI_TEST_VAR) = {}\n", os.env("CHI_TEST_VAR"));
    printf("env(NONEXISTENT_XYZ_123) = {}\n", os.env("NONEXISTENT_XYZ_123"));

    // cwd returns a non-empty string
    var dir = os.cwd();
    printf("cwd is non-empty = {}\n", dir != "");
}

