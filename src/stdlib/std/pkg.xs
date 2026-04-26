// std/pkg — package metadata for the binary being compiled.
//
// Values are baked in at build time by chic from the package's package.jsonc.
// Any field not provided is null.

export struct PackageInfo {
    name: ?string = null;
    version: ?string = null;
    description: ?string = null;
}

extern "C" {
    unsafe func __cx_pkg_name() *string;
    unsafe func __cx_pkg_version() *string;
    unsafe func __cx_pkg_description() *string;
}

unsafe func deref_optional(p: *string) ?string {
    return p ? *p : null;
}

export func info() PackageInfo {
    unsafe {
        return PackageInfo{
            name: deref_optional(__cx_pkg_name()),
            version: deref_optional(__cx_pkg_version()),
            description: deref_optional(__cx_pkg_description()),
        };
    }
}
