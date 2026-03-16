// Module that re-exports C string functions using wildcard pattern
extern "C" {
    export {str*} from "string.h";
}
