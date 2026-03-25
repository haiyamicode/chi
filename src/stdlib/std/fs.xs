// std/fs — filesystem operations
import "std/filepath" as filepath;
import "std/io" as io;
import "std/ops" as ops;

extern "C" {
    unsafe func __cx_fs_error_kind(uv_err: int32) int32;
    unsafe func __cx_fs_flags(which: int32) int32;
    unsafe func __cx_fs_open(path: *byte, flags: int32, mode: int32) int32;
    unsafe func __cx_fs_read(fd: int32, buf: *void, size: uint32) int32;
    unsafe func __cx_fs_read_async(
        fd: int32,
        buf: *void,
        size: uint32,
        resolve: *void,
        reject: *void
    );
    unsafe func __cx_fs_write(fd: int32, data: *void, size: uint32) int32;
    unsafe func __cx_fs_write_async(
        fd: int32,
        data: *void,
        size: uint32,
        resolve: *void,
        reject: *void
    );
    unsafe func __cx_fs_close(fd: int32) int32;
    unsafe func __cx_file_exists(path: *byte) int32;
    unsafe func __cx_is_dir(path: *byte) int32;
    unsafe func __cx_file_remove(path: *byte) int32;
    unsafe func __cx_dir_remove(path: *byte) int32;
    unsafe func __cx_copy_file(src: *byte, dest: *byte) int32;
    unsafe func __cx_mkdir(path: *byte) int32;
    unsafe func __cx_list_dir(path: *byte, result: *void) int32;
    unsafe func __cx_glob(base: *byte, pattern: *byte, result: *void) int32;
    unsafe func __cx_uv_strerror(errnum: int32, result: *string);
}

func uv_strerror(code: int32) string {
    let result = "";
    unsafe {
        __cx_uv_strerror(code, &result);
    }
    return result;
}

export enum ErrorKind {
    Unknown,
    NotFound,
    PermissionDenied,
    AlreadyExists,
    NotADirectory,
    IsADirectory,
    DirectoryNotEmpty,
    NoSpace,
    ReadOnlyFs,
    Busy
}

func error_kind_from(uv_err: int32) ErrorKind {
    unsafe {
        return __cx_fs_error_kind(uv_err) as ErrorKind;
    }
}

export struct FsError {
    kind: ErrorKind = ErrorKind.Unknown;
    op: string = "";
    path: string = "";
    raw_code: int32 = 0;
    detail: string = "";

    impl Error {
        func message() string {
            return stringf("{} {}: {} ({})", this.op, this.path, this.detail, this.raw_code);
        }
    }
}

func throw_fs_error(op: string, path: string, code: int32) never {
    throw new FsError{
        kind: error_kind_from(code),
        :op,
        :path,
        raw_code: code,
        detail: uv_strerror(code)
    };
}

// POSIX open flags (platform-dependent values from runtime)
func fs_flag(which: int32) int32 {
    unsafe {
        return __cx_fs_flags(which);
    }
}

let O_RDONLY: int32 = fs_flag(0);
let O_WRONLY: int32 = fs_flag(1);
let O_RDWR: int32 = fs_flag(2);
let O_CREAT: int32 = fs_flag(3);
let O_TRUNC: int32 = fs_flag(4);
let O_APPEND: int32 = fs_flag(5);

export enum OpenMode {
    Read,
    Write,
    Append,
    ReadWrite,
    WriteRead;

    struct {
        func flags() int32 {
            return switch this {
                Read => O_RDONLY,
                Write => O_WRONLY | O_CREAT | O_TRUNC,
                Append => O_WRONLY | O_CREAT | O_APPEND,
                ReadWrite => O_RDWR,
                WriteRead => O_RDWR | O_CREAT | O_TRUNC,
                else => O_RDONLY
            };
        }
    }
}

struct FileHandle {
    private fd: int32 = -1;
    private path: string = "";

    mut func new(fd: int32, path: string) {
        this.fd = fd;
        this.path = move path;
    }

    mut func close() {
        if this.fd >= 0 {
            unsafe {
                __cx_fs_close(this.fd);
            }
            this.fd = -1;
        }
    }

    func read(buf: &mut [byte]) uint32 {
        unsafe {
            let n = __cx_fs_read(this.fd, buf.as_ptr(), buf.length);
            if n < 0 {
                return 0;
            }
            return n as uint32;
        }
    }

    func write(data: &[byte]) int32 {
        unsafe {
            return __cx_fs_write(this.fd, data.as_ptr(), data.length);
        }
    }

    func raw_fd() int32 {
        return this.fd;
    }

    func file_path() string {
        return this.path;
    }

    mut func delete() {
        this.close();
    }

    impl ops.NoCopy {}
}

export struct File {
    private handle: Shared<FileHandle>;

    private mut func new(fd: int32, path: string) {
        this.handle = {new {fd, move path}};
    }

    static func open(path: string, mode: OpenMode = OpenMode.Read) File {
        var cs = path.to_cstring();
        unsafe {
            var fd = __cx_fs_open(cs.as_ptr(), mode.flags(), 438); // 438 = 0o666
            if fd < 0 {
                throw_fs_error("open", path, fd);
            }
            return {fd, path};
        }
    }

    static func create(path: string) File {
        return File.open(path, OpenMode.Write);
    }

    impl io.Read {
        mut func read(buf: &mut [byte]) uint32 {
            return this.handle.read(buf);
        }
    }

    impl io.Write {
        func write(data: &[byte]) {
            this.handle.write(data);
        }
    }

    impl io.Close {
        mut func close() {
            this.handle.close();
        }
    }

    func async() FileAsync {
        return {this.handle};
    }
}

export struct FileAsync {
    private handle: Shared<FileHandle>;

    mut func new(handle: Shared<FileHandle>) {
        this.handle = move handle;
    }

    impl io.ReadAsync {
        func read(buf: &mut [byte]) Promise<uint32> {
            var promise = Promise<uint32>{};
            var path = this.handle.file_path();
            var resolve = func [promise] (num_bytes: uint32) {
                promise.resolve(num_bytes);
            };
            var reject = func [promise, path] (err: int32) {
                promise.reject(
                    new FsError{
                        kind: error_kind_from(err),
                        op: "read",
                        :path,
                        raw_code: err,
                        detail: uv_strerror(err)
                    }
                );
            };
            unsafe {
                __cx_fs_read_async(
                    this.handle.raw_fd(),
                    buf.as_ptr(),
                    buf.length,
                    &resolve,
                    &reject
                );
            }
            return promise;
        }
    }

    impl io.WriteAsync {
        func write(data: &[byte]) Promise {
            var promise = Promise{};
            var path = this.handle.file_path();
            var resolve = func [promise] () {
                promise.resolve(());
            };
            var reject = func [promise, path] (err: int32) {
                promise.reject(
                    new FsError{
                        kind: error_kind_from(err),
                        op: "write",
                        :path,
                        raw_code: err,
                        detail: uv_strerror(err)
                    }
                );
            };
            unsafe {
                __cx_fs_write_async(
                    this.handle.raw_fd(),
                    data.as_ptr(),
                    data.length,
                    &resolve,
                    &reject
                );
            }
            return promise;
        }
    }
}

export func read_file(path: string) string {
    var f = File.open(path);
    return f.read_string();
}

export func write_file(path: string, data: string) {
    var f = File.create(path);
    f.write_string(data);
}

export func append_file(path: string, data: string) {
    var f = File.open(path, OpenMode.Append);
    f.write_string(data);
}

export async func read_file_async(path: string) Promise<string> {
    var f = File.open(path);
    return await f.async().read_string();
}

export async func write_file_async(path: string, data: string) Promise {
    var f = File.create(path);
    await f.async().write_string(data);
}

export async func append_file_async(path: string, data: string) Promise {
    var f = File.open(path, OpenMode.Append);
    await f.async().write_string(data);
}

export func exists(path: string) bool {
    var cs = path.to_cstring();
    unsafe {
        return __cx_file_exists(cs.as_ptr()) != 0;
    }
}

export func is_dir(path: string) bool {
    var cs = path.to_cstring();
    unsafe {
        return __cx_is_dir(cs.as_ptr()) != 0;
    }
}

export func remove(path: string) {
    var cs = path.to_cstring();
    unsafe {
        let r = __cx_file_remove(cs.as_ptr());
        if r != 0 {
            throw_fs_error("remove", path, r);
        }
    }
}

func remove_dir(path: string) {
    var cs = path.to_cstring();
    unsafe {
        let r = __cx_dir_remove(cs.as_ptr());
        if r != 0 {
            throw_fs_error("rmdir", path, r);
        }
    }
}

export func remove_all(path: string) {
    if !exists(path) {
        return;
    }
    if is_dir(path) {
        let entries = list_dir(path);
        for entry in entries {
            remove_all(filepath.join(path, entry));
        }
        remove_dir(path);
        return;
    }
    remove(path);
}

export func mkdir(path: string) {
    var cs = path.to_cstring();
    unsafe {
        let r = __cx_mkdir(cs.as_ptr());
        if r != 0 {
            throw_fs_error("mkdir", path, r);
        }
    }
}

export func copy_file(src: string, dest: string) {
    var src_cs = src.to_cstring();
    var dest_cs = dest.to_cstring();
    unsafe {
        let r = __cx_copy_file(src_cs.as_ptr(), dest_cs.as_ptr());
        if r != 0 {
            throw_fs_error("copy", stringf("{} -> {}", src, dest), r);
        }
    }
}

export func copy_all(src: string, dest: string) {
    if is_dir(src) {
        mkdir_all(dest);
        let entries = list_dir(src);
        for entry in entries {
            let child_src = filepath.join(src, entry);
            let child_dest = filepath.join(dest, entry);
            copy_all(child_src, child_dest);
        }
        return;
    }
    copy_file(src, dest);
}

export func mkdir_all(path: string) {
    for var i: uint32 = 1; i < path.byte_length(); i = i + 1 {
        if path.byte_at(i) == filepath.PATH_SEPARATOR {
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
        let r = __cx_list_dir(cs.as_ptr(), &result);
        if r != 0 {
            throw_fs_error("list_dir", path, r);
        }
    }
    return result;
}

export func glob(pattern: string, base: string = ".") Array<string> {
    var result: Array<string> = [];
    var base_cs = base.to_cstring();
    var pattern_cs = pattern.to_cstring();
    unsafe {
        let r = __cx_glob(base_cs.as_ptr(), pattern_cs.as_ptr(), &result);
        if r != 0 {
            throw_fs_error("glob", filepath.join(base, pattern), r);
        }
    }
    return result;
}
