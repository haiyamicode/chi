// Broken import/export syntax - parser should not crash

// Missing 'as' keyword after *
import * "./test";

// Unclosed braces in named imports
import {X, Y ;

// Missing 'from' keyword
import * as mod;

// Wrong order
import {X} "./test";

// Missing everything
import ;

// Missing module path
import * as mod from ;

// Unclosed export braces
export {X, Y ;

// Missing 'from' in export
export * ;

func main() {}
