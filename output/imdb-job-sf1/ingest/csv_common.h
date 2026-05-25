#pragma once
// Common helpers for IMDB JOB CSV ingestion.
//
// CSV format (PostgreSQL COPY ... WITH CSV ESCAPE '\'):
//   - Fields separated by ','
//   - Optional double-quote wrapping a field. Inside a quoted field, `\X` (backslash + ANY char)
//     is an escape: the literal character X. Most common are `\"` and `\\`.
//   - Newlines may appear inside quoted fields (rare, but observed)
//   - An empty field (between commas, or empty quoted "") means SQL NULL
//
// On-disk layout:
//   <table>/<col>.bin                  raw little-endian array of cpp_type
//   <table>/<col>.offsets.bin          uint64[N+1] for varlen columns
//   <table>/<col>.data.bin             raw bytes for varlen columns
//   <table>/__row_count.bin            uint64
//
// All output values are written in CSV order (no sorting here — done by build_indexes).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ---------------- mmap helper ----------------
struct MMapFile {
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { std::fprintf(stderr, "open(%s): %s\n", path.c_str(), strerror(errno)); return false; }
        struct stat st;
        if (fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat(%s)\n", path.c_str()); return false; }
        size = st.st_size;
        if (size == 0) { data = nullptr; return true; }
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { std::fprintf(stderr, "mmap(%s)\n", path.c_str()); return false; }
        data = static_cast<const char*>(p);
        madvise(const_cast<void*>(static_cast<const void*>(data)), size, MADV_SEQUENTIAL);
        return true;
    }
    ~MMapFile() {
        if (data && size > 0) munmap(const_cast<void*>(static_cast<const void*>(data)), size);
        if (fd >= 0) ::close(fd);
    }
};

// ---------------- write helpers ----------------
inline void write_file(const std::string& path, const void* data, size_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "fopen(%s): %s\n", path.c_str(), strerror(errno)); std::exit(1); }
    if (bytes && std::fwrite(data, 1, bytes, f) != bytes) {
        std::fprintf(stderr, "fwrite(%s) failed\n", path.c_str()); std::exit(1);
    }
    std::fclose(f);
}

template <class T>
inline void write_vec(const std::string& path, const std::vector<T>& v) {
    write_file(path, v.data(), v.size() * sizeof(T));
}

// ---------------- column buffers ----------------
enum class ColType { INT32, INT32_NULL, CHAR1_NULL, VARLEN, SKIP };

struct ColBuf {
    ColType type;
    std::string name;
    std::vector<int32_t> i32;     // for INT32 / INT32_NULL
    std::vector<char>    ch1;     // for CHAR1_NULL
    std::vector<uint64_t> off;    // for VARLEN (starts with one 0)
    std::vector<char>     dat;    // for VARLEN
};

inline ColBuf make_col(ColType t, std::string name) {
    ColBuf c;
    c.type = t;
    c.name = std::move(name);
    if (t == ColType::VARLEN) c.off.push_back(0);
    return c;
}

inline void col_reserve(ColBuf& c, size_t n_rows) {
    switch (c.type) {
        case ColType::INT32: case ColType::INT32_NULL: c.i32.reserve(n_rows); break;
        case ColType::CHAR1_NULL: c.ch1.reserve(n_rows); break;
        case ColType::VARLEN: c.off.reserve(n_rows + 1); c.dat.reserve(n_rows * 16); break;
        case ColType::SKIP: break;
    }
}

// Append parsed field value into column buffer.
// `s`/`len` is the raw (already-unquoted) field bytes; `is_null` indicates SQL NULL.
inline void col_append(ColBuf& c, const char* s, size_t len, bool is_null) {
    switch (c.type) {
        case ColType::INT32: {
            // Should not be null; parse signed integer
            int32_t v = 0; const char* p = s; const char* e = s + len;
            bool neg = false;
            if (p < e && (*p == '-' || *p == '+')) { neg = (*p == '-'); ++p; }
            for (; p < e; ++p) v = v * 10 + (*p - '0');
            c.i32.push_back(neg ? -v : v);
            break;
        }
        case ColType::INT32_NULL: {
            if (is_null) { c.i32.push_back(-1); break; }
            int32_t v = 0; const char* p = s; const char* e = s + len;
            bool neg = false;
            if (p < e && (*p == '-' || *p == '+')) { neg = (*p == '-'); ++p; }
            for (; p < e; ++p) v = v * 10 + (*p - '0');
            c.i32.push_back(neg ? -v : v);
            break;
        }
        case ColType::CHAR1_NULL: {
            c.ch1.push_back(is_null || len == 0 ? '\0' : s[0]);
            break;
        }
        case ColType::VARLEN: {
            if (!is_null && len > 0) c.dat.insert(c.dat.end(), s, s + len);
            c.off.push_back((uint64_t)c.dat.size());
            break;
        }
        case ColType::SKIP: break;
    }
}

// ---------------- CSV parser ----------------
// Parses a single row starting at *pp. On return *pp points past the row's terminating newline
// (or to end). `cb(field_idx, ptr, len, is_null)` is invoked for each field in CSV order.
// `scratch` is reused for quoted fields containing escape sequences.
//
// Returns false if *pp == end (no row to parse).
template <class FieldCb>
inline bool parse_row(const char*& pp, const char* end, std::vector<char>& scratch, FieldCb cb) {
    if (pp >= end) return false;
    const char* p = pp;
    int field_idx = 0;
    while (p < end) {
        // Parse one field
        const char* fs;
        size_t flen;
        bool is_null;
        if (*p == '"') {
            // Quoted: scan to matching unescaped ".  Inside, `\X` is an escape: literal X.
            ++p;
            const char* qs = p;
            const char* q = p;
            bool has_escape = false;
            for (;;) {
                // Scan for either backslash (escape) or closing quote.
                while (q < end && *q != '"' && *q != '\\') ++q;
                if (q >= end) { fs = qs; flen = (size_t)(q - qs); break; }
                if (*q == '\\') {
                    has_escape = true;
                    q += 2;          // skip backslash + escaped char
                    if (q > end) q = end;
                    continue;
                }
                // closing unescaped quote
                fs = qs; flen = (size_t)(q - qs);
                break;
            }
            if (has_escape) {
                scratch.clear();
                scratch.reserve(flen);
                const char* r = qs; const char* re = qs + flen;
                while (r < re) {
                    if (*r == '\\' && r + 1 < re) { scratch.push_back(r[1]); r += 2; }
                    else { scratch.push_back(*r); ++r; }
                }
                fs = scratch.data();
                flen = scratch.size();
            }
            is_null = false;
            p = q < end ? q + 1 : q;  // step past closing quote
        } else {
            // Unquoted: scan to comma or newline
            const char* qs = p;
            while (p < end && *p != ',' && *p != '\n' && *p != '\r') ++p;
            fs = qs;
            flen = (size_t)(p - qs);
            is_null = (flen == 0);
        }
        cb(field_idx, fs, flen, is_null);
        ++field_idx;
        if (p >= end) break;
        if (*p == ',') { ++p; continue; }
        // newline (or \r\n)
        if (*p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
        break;
    }
    pp = p;
    return true;
}

// ---------------- table parsing driver ----------------
struct TableSpec {
    std::string name;
    std::string csv_filename;
    std::vector<std::string> csv_cols;     // names in CSV order
    std::vector<ColBuf>      out_cols;     // output columns to keep, in arbitrary order
    // computed: for each csv column index, the index into out_cols (or -1 to skip)
    std::vector<int>         csv_to_out;
};

inline void prep_table(TableSpec& t) {
    t.csv_to_out.assign(t.csv_cols.size(), -1);
    for (size_t i = 0; i < t.csv_cols.size(); ++i) {
        for (size_t j = 0; j < t.out_cols.size(); ++j) {
            if (t.out_cols[j].name == t.csv_cols[i]) { t.csv_to_out[i] = (int)j; break; }
        }
    }
}

inline uint64_t parse_table(TableSpec& t, const std::string& csv_dir) {
    std::string csv_path = csv_dir + "/" + t.csv_filename;
    MMapFile m;
    if (!m.open(csv_path)) std::exit(1);

    // Reserve buffers
    for (auto& c : t.out_cols) col_reserve(c, /*hint*/ 1024);

    std::vector<char> scratch;
    scratch.reserve(256);
    const char* p = m.data;
    const char* end = m.data + m.size;
    uint64_t row_count = 0;

    while (p < end) {
        // Skip blank lines defensively
        if (*p == '\n') { ++p; continue; }
        parse_row(p, end, scratch, [&](int field_idx, const char* fs, size_t flen, bool is_null) {
            if (field_idx >= (int)t.csv_to_out.size()) return;  // extra field, ignore
            int out_idx = t.csv_to_out[field_idx];
            if (out_idx < 0) return;
            col_append(t.out_cols[out_idx], fs, flen, is_null);
        });
        // Backfill missing fields (if row had fewer than expected) — treat as NULL for kept cols
        // (defensive; IMDB CSVs are well-formed)
        ++row_count;
    }
    return row_count;
}

inline void write_table(const TableSpec& t, uint64_t row_count, const std::string& storage_dir) {
    std::string dir = storage_dir + "/" + t.name;
    fs::create_directories(dir);
    uint64_t rc = row_count;
    write_file(dir + "/__row_count.bin", &rc, sizeof(rc));
    for (const auto& c : t.out_cols) {
        std::string base = dir + "/" + c.name;
        switch (c.type) {
            case ColType::INT32: case ColType::INT32_NULL:
                write_vec(base + ".bin", c.i32); break;
            case ColType::CHAR1_NULL:
                write_vec(base + ".bin", c.ch1); break;
            case ColType::VARLEN:
                write_vec(base + ".offsets.bin", c.off);
                write_vec(base + ".data.bin", c.dat);
                break;
            case ColType::SKIP: break;
        }
    }
}
