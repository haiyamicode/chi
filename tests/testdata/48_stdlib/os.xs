import "std/os" as os;

func main() {
    // set and get env
    os.set_env("CHI_TEST_VAR", "hello_chi");
    printf("env(CHI_TEST_VAR) = {}\n", os.env("CHI_TEST_VAR"));
    printf("env(NONEXISTENT_XYZ_123) = {}\n", os.env("NONEXISTENT_XYZ_123"));

    // cwd returns a non-empty string
    var dir = os.cwd();
    printf("cwd is non-empty = {}\n", dir != "");

    @[if(platform.unix)] {
        let system_result = os.system("printf 'sysout'; printf 'syserr' >&2; exit 7");
        printf("system exit = {}\n", system_result.exit_code);
        printf("system ok = {}\n", system_result.is_ok());
        printf("system stdout = {}\n", system_result.stdout);
        printf("system stderr = {}\n", system_result.stderr);

        let command_args = ["sh", "-c", "printf 'cmdout'; printf 'cmderr' >&2; exit 5"];
        let command_result = os.command(command_args.span());
        printf("command exit = {}\n", command_result.exit_code);
        printf("command ok = {}\n", command_result.is_ok());
        printf("command stdout = {}\n", command_result.stdout);
        printf("command stderr = {}\n", command_result.stderr);
    }
}
