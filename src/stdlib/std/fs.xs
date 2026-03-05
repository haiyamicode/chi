// std/fs — filesystem operations
import "std/io" as io;
import "std/ops" as ops;

extern "C" {
    unsafe func __cx_fopen(path: *byte, mode: *byte) *void;
    unsafe func __cx_fread(handle: *void, buf: *void, size: uint32) uint32;
    unsafe func __cx_fwrite(handle: *void, data: *void, size: uint32) uint32;
    unsafe func __cx_fclose(handle: *void);
    unsafe func __cx_file_exists(path: *byte) int32;
    unsafe func __cx_file_remove(path: *byte) int32;
    unsafe func __cx_mkdir(path: *byte) int32;
    unsafe func __cx_list_dir(path: *byte, result: *void) int32;
}

export enum OpenMode {
    Read,
    Write,
    Append,
    ReadWrite
}

func mode_str(mode: OpenMode) string {
    return switch mode {
        OpenMode.Read => "r",
        OpenMode.Write => "w",
        OpenMode.Append => "a",
        OpenMode.ReadWrite => "r+",
        else => "r"
    };
}

export struct File {
    private handle: *void;

    private mut func new(handle: *void) {
        this.handle = handle;
    }

    static func open(path: string, mode: OpenMode = OpenMode.Read) File {
        var p = path.to_cstring();
        var m = mode_str(mode).to_cstring();
        unsafe {
            var h = __cx_fopen(p.as_ptr(), m.as_ptr());
            if h == null {
                panic(stringf("open failed: {}", path));
            }
            return {h};
        }
    }

    static func create(path: string) File {
        return File.open(path, OpenMode.Write);
    }

    impl io.Reader {
        mut func read(buf: []mut byte) uint32 {
            unsafe {
                return __cx_fread(this.handle, buf.as_ptr(), buf.length);
            }
        }
    }

    impl io.Writer {
        func write(data: []byte) {
            unsafe {
                __cx_fwrite(this.handle, data.as_ptr(), data.length);
            }
        }
    }

    impl io.Closer {
        mut func close() {
            if this.handle != null {
                unsafe {
                    __cx_fclose(this.handle);
                }
                this.handle = null;
            }
        }
    }

    mut func delete() {
        this.close();
    }

    impl ops.DisallowCopy {}
}

export func read_file(path: string) string {
    var f = File.open(path);
    var content = f.read_string();
    f.close();
    return content;
}

export func write_file(path: string, data: string) {
    var f = File.create(path);
    f.write_string(data);
    f.close();
}

export func append_file(path: string, data: string) {
    var f = File.open(path, OpenMode.Append);
    f.write_string(data);
    f.close();
}

export func exists(path: string) bool {
    var cs = path.to_cstring();
    unsafe {
        return __cx_file_exists(cs.as_ptr()) != 0;
    }
}

export func remove(path: string) {
    var cs = path.to_cstring();
    unsafe {
        if __cx_file_remove(cs.as_ptr()) != 0 {
            panic(stringf("remove failed: {}", path));
        }
    }
}

export func mkdir(path: string) {
    var cs = path.to_cstring();
    unsafe {
        if __cx_mkdir(cs.as_ptr()) != 0 {
            panic(stringf("mkdir failed: {}", path));
        }
    }
}

export func mkdir_all(path: string) {
    for var i: uint32 = 1; i < path.byte_length(); i = i + 1 {
        if path.byte_at(i) == '/' {
            var component = path.byte_slice(0, i);
            if !exists(component) {
                mkdir(component);
            }
        }
    }
    if !exists(path) {
        mkdir(path);
    }
}

export func list_dir(path: string) Array<string> {
    var result: Array<string> = [];
    var cs = path.to_cstring();
    unsafe {
        if __cx_list_dir(cs.as_ptr(), &result) != 0 {
            panic(stringf("list_dir failed: {}", path));
        }
    }
    return result;
}

