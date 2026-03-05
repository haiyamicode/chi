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
    unsafe func __cx_get_errno() int32;
    unsafe func __cx_strerror(errnum: int32, result: *string);
}

func get_errno() int32 {
    unsafe {
        return __cx_get_errno();
    }
}

func strerror(errnum: int32) string {
    let result = "";
    unsafe {
        __cx_strerror(errnum, &result);
    }
    return result;
}

export struct FsError {
    op: string = "";
    path: string = "";
    code: int32 = 0;
    detail: string = "";

    impl Error {
        func message() string {
            return stringf("{} {}: {} (errno {})", this.op, this.path, this.detail, this.code);
        }
    }
}

func throw_fs_error(op: string, path: string) never {
    let code = get_errno();
    throw new FsError{
        op: op,
        path: path,
        code: code,
        detail: strerror(code)
    };
}

export enum OpenMode {
    Read,
    Write,
    Append,
    ReadWrite,
    WriteRead;

    struct {
        func mode_string() string {
            return switch this {
                OpenMode.Read => "rb",
                OpenMode.Write => "wb",
                OpenMode.Append => "ab",
                OpenMode.ReadWrite => "r+b",
                OpenMode.WriteRead => "w+b",
                else => "rb"
            };
        }
    }
}

struct FileHandle {
    private raw: *void = null;

    mut func new(raw: *void) {
        this.raw = raw;
    }

    mut func close() {
        if this.raw != null {
            unsafe {
                __cx_fclose(this.raw);
            }
            this.raw = null;
        }
    }

    func read(buf: []mut byte) uint32 {
        unsafe {
            return __cx_fread(this.raw, buf.as_ptr(), buf.length);
        }
    }

    func write(data: []byte) {
        unsafe {
            __cx_fwrite(this.raw, data.as_ptr(), data.length);
        }
    }

    mut func delete() {
        this.close();
    }

    impl ops.DisallowCopy {}
}

export struct File {
    private handle: Shared<FileHandle>;

    private mut func new(raw: *void) {
        this.handle = {FileHandle{raw}};
    }

    static func open(path: string, mode: OpenMode = OpenMode.Read) File {
        var p = path.to_cstring();
        var m = mode.mode_string().to_cstring();
        unsafe {
            var h = __cx_fopen(p.as_ptr(), m.as_ptr());
            if h == null {
                throw_fs_error("open", path);
            }
            return {h};
        }
    }

    static func create(path: string) File {
        return File.open(path, OpenMode.Write);
    }

    impl io.Reader {
        mut func read(buf: []mut byte) uint32 {
            return this.handle.read(buf);
        }
    }

    impl io.Writer {
        func write(data: []byte) {
            this.handle.write(data);
        }
    }

    impl io.Closer {
        mut func close() {
            this.handle.close();
        }
    }
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
            throw_fs_error("remove", path);
        }
    }
}

export func mkdir(path: string) {
    var cs = path.to_cstring();
    unsafe {
        if __cx_mkdir(cs.as_ptr()) != 0 {
            throw_fs_error("mkdir", path);
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
            throw_fs_error("list_dir", path);
        }
    }
    return result;
}
