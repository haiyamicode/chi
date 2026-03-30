import "std/os" as os;

func contains(xs: &[string], needle: string) bool {
    for x in xs {
        if x == needle {
            return true;
        }
    }
    return false;
}

func main() {
    let tags = os.platform_tags();
    println("testing conditional compilation:");
    @[if(platform.windows)] {
        assert(contains(tags.span(), "platform.windows"));
        println("os-tag");
    }
    @[if(platform.linux)] {
        assert(contains(tags.span(), "platform.linux"));
        println("os-tag");
    }
    @[if(platform.macos)] {
        assert(contains(tags.span(), "platform.macos"));
        println("os-tag");
    }
    @[if(platform.unix)] {
        assert(contains(tags.span(), "platform.unix"));
        println("family-tag");
    }
    @[if(platform.windows)] {
        assert(contains(tags.span(), "platform.windows"));
        println("family-tag");
    }
    @[if(arch.x64)] {
        assert(contains(tags.span(), "arch.x64"));
        println("arch-tag");
    }
    @[if(arch.arm64)] {
        assert(contains(tags.span(), "arch.arm64"));
        println("arch-tag");
    }
    @[if(arch.x86)] {
        assert(contains(tags.span(), "arch.x86"));
        println("arch-tag");
    }
    @[if(arch.arm)] {
        assert(contains(tags.span(), "arch.arm"));
        println("arch-tag");
    }
}
