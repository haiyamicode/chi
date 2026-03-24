enum ParseStep {
    Ok,
    Help(string),
    Error(string)
}

enum Parse {
    Ok,
    Help {
        help_text: string;
    },
    Error {
        error_text: string;
    }
}

func finalize_parse(step: ParseStep) Parse {
    switch step {
        Ok => {},
        Help(text) => {
            return Help{text};
        },
        Error(text) => {
            return Error{text};
        }
    }
    return Ok{};
}

func main() {
    let help = finalize_parse(Help{"usage"});
    let err = finalize_parse(Error{"bad"});
    let ok = finalize_parse(Ok{});
    println(switch help {
        Help => help.help_text,
        else => "bad help"
    });
    println(switch err {
        Error => err.error_text,
        else => "bad err"
    });
    println(switch ok {
        Ok => "ok",
        else => "not ok"
    });
}
