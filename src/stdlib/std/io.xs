// std/io — core I/O interfaces

export const DEFAULT_BUF_SIZE: uint32 = 8192;

export interface Read {
    func read(buf: []mut byte) uint32;

    func read_bytes(n: uint32) Buffer {
        var buf = Buffer.alloc(n);
        var num_bytes = this.read(buf.span_mut());
        buf.truncate(num_bytes);
        return buf;
    }

    func read_all() Buffer {
        var result = Buffer{};
        var chunk_size: uint32 = 32;
        var chunk = Buffer.alloc(chunk_size);
        while true {
            var n = this.read(chunk.span_mut());
            if n == 0 {
                break;
            }
            result.write(chunk.span(0, n));
            if chunk_size < DEFAULT_BUF_SIZE {
                chunk_size *= 2;
                if chunk_size > DEFAULT_BUF_SIZE {
                    chunk_size = DEFAULT_BUF_SIZE;
                }
                chunk = Buffer.alloc(chunk_size);
            }
        }
        return result;
    }

    func read_string() string {
        return this.read_all().to_string();
    }
}

export interface Write {
    func write(data: []byte);

    func write_string(text: string) {
        this.write(text.byte_span());
    }
}

export interface Close {
    func close();
}

