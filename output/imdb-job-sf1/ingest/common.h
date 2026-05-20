// Common utilities for IMDB-JOB ingest & index build
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

constexpr int32_t NULL_INT = INT32_MIN;

// --- mmap a whole file read-only -----------------------------------------------
struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;
    void unmap() {
        if (data) { munmap((void*)data, size); data = nullptr; }
        if (fd >= 0) { close(fd); fd = -1; }
    }
    ~MappedFile() { unmap(); }
};

inline MappedFile mmap_file(const std::string& path) {
    MappedFile m;
    m.fd = open(path.c_str(), O_RDONLY);
    if (m.fd < 0) { fprintf(stderr, "open failed: %s\n", path.c_str()); exit(1); }
    struct stat st;
    fstat(m.fd, &st);
    m.size = st.st_size;
    if (m.size == 0) { m.data = ""; return m; }
    void* p = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.fd, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "mmap failed: %s\n", path.c_str()); exit(1); }
    madvise(p, m.size, MADV_SEQUENTIAL);
    m.data = (const char*)p;
    return m;
}

// --- raw binary read/write -----------------------------------------------------
inline void write_file(const std::string& path, const void* data, size_t bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "write open failed: %s\n", path.c_str()); exit(1); }
    if (bytes > 0 && fwrite(data, 1, bytes, f) != bytes) {
        fprintf(stderr, "write failed: %s\n", path.c_str()); exit(1);
    }
    fclose(f);
}

template <typename T>
inline void write_vec(const std::string& path, const std::vector<T>& v) {
    write_file(path, v.data(), v.size() * sizeof(T));
}

inline std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "read open failed: %s\n", path.c_str()); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(sz);
    if (sz > 0 && fread(v.data(), 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed: %s\n", path.c_str()); exit(1);
    }
    fclose(f); return v;
}

template <typename T>
inline std::vector<T> read_vec(const std::string& path) {
    auto raw = read_file(path);
    assert(raw.size() % sizeof(T) == 0);
    std::vector<T> v(raw.size() / sizeof(T));
    if (!v.empty()) memcpy(v.data(), raw.data(), raw.size());
    return v;
}

inline void mkdirp(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
            if (i < path.size()) cur += '/';
        } else { cur += path[i]; }
    }
}

// --- CSV parser (PostgreSQL COPY CSV with backslash-escaped quotes) ------------
// Reads one record. Returns number of fields. Each field is (start,len) within
// scratch buffer if unescaped; otherwise the field is copied into scratch with
// escape resolution. We hand back string_views; the underlying memory is owned
// by either the mmap or by `scratch`. Callers must consume views before the
// next call to parse_record overwrites scratch.

struct CsvParser {
    const char* p;     // current position
    const char* end;   // end of file
    std::string scratch;        // grows; we reset its size for each record
    std::vector<std::string_view> fields;  // reused

    void init(const char* data, size_t n) {
        p = data; end = data + n;
        scratch.reserve(1 << 20);
        fields.reserve(32);
    }

    bool at_eof() const { return p >= end; }

    // Returns false at EOF (no more records). Otherwise fills `fields`.
    bool next() {
        if (p >= end) return false;
        fields.clear();
        scratch.clear();
        // For each field, remember (offset_in_scratch, len). After the record
        // is fully read scratch won't reallocate as long as we reserved enough.
        // We'll re-derive string_views at the end from offsets.
        static thread_local std::vector<uint32_t> off_lens; // pairs (off,len)
        off_lens.clear();
        while (true) {
            uint32_t fstart = (uint32_t)scratch.size();
            if (p < end && *p == '"') {
                // quoted field
                ++p;
                while (p < end) {
                    char c = *p;
                    if (c == '\\' && p + 1 < end) {
                        char n = p[1];
                        if (n == '"' || n == '\\') { scratch.push_back(n); p += 2; continue; }
                        // Other backslash sequences: keep as-is.
                        scratch.push_back('\\'); scratch.push_back(n); p += 2; continue;
                    }
                    if (c == '"') {
                        // doubled quote escape? Some psql dumps use "" too.
                        if (p + 1 < end && p[1] == '"') {
                            scratch.push_back('"'); p += 2; continue;
                        }
                        ++p; break;  // end of quoted field
                    }
                    scratch.push_back(c);
                    ++p;
                }
            } else {
                // unquoted field
                while (p < end) {
                    char c = *p;
                    if (c == ',' || c == '\n' || c == '\r') break;
                    scratch.push_back(c);
                    ++p;
                }
            }
            uint32_t flen = (uint32_t)scratch.size() - fstart;
            off_lens.push_back(fstart);
            off_lens.push_back(flen);
            if (p >= end) break;
            if (*p == ',') { ++p; continue; }
            if (*p == '\r') ++p;
            if (p < end && *p == '\n') { ++p; }
            break;
        }
        fields.resize(off_lens.size() / 2);
        const char* base = scratch.data();
        for (size_t i = 0; i < fields.size(); ++i) {
            fields[i] = std::string_view(base + off_lens[2*i], off_lens[2*i+1]);
        }
        return true;
    }
};

// --- int parsing (returns NULL_INT if empty) ----------------------------------
inline int32_t parse_int_or_null(std::string_view sv) {
    if (sv.empty()) return NULL_INT;
    bool neg = false;
    size_t i = 0;
    if (sv[0] == '-') { neg = true; i = 1; }
    int64_t v = 0;
    for (; i < sv.size(); ++i) {
        char c = sv[i];
        if (c < '0' || c > '9') return NULL_INT;  // malformed
        v = v * 10 + (c - '0');
    }
    return (int32_t)(neg ? -v : v);
}

// --- varlen column writer ------------------------------------------------------
struct VarlenCol {
    std::vector<int64_t> offsets;  // size N+1
    std::vector<char> data;
    VarlenCol(size_t reserve_rows = 0, size_t reserve_bytes = 0) {
        if (reserve_rows) offsets.reserve(reserve_rows + 1);
        if (reserve_bytes) data.reserve(reserve_bytes);
        offsets.push_back(0);
    }
    void append(std::string_view sv) {
        data.insert(data.end(), sv.begin(), sv.end());
        offsets.push_back((int64_t)data.size());
    }
    void write(const std::string& dir, const std::string& col) const {
        write_vec(dir + "/" + col + ".off", offsets);
        write_file(dir + "/" + col + ".dat", data.data(), data.size());
    }
    // Apply permutation perm[i] = original row index; produce reordered varlen.
    VarlenCol reorder(const std::vector<int32_t>& perm) const {
        VarlenCol out;
        out.offsets.reserve(perm.size() + 1);
        out.data.reserve(data.size());
        for (size_t i = 0; i < perm.size(); ++i) {
            int32_t r = perm[i];
            int64_t s = offsets[r], e = offsets[r+1];
            out.data.insert(out.data.end(), data.data() + s, data.data() + e);
            out.offsets.push_back((int64_t)out.data.size());
        }
        return out;
    }
};

// --- progress logger -----------------------------------------------------------
inline std::mutex g_log_mu;
inline void logmsg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}
