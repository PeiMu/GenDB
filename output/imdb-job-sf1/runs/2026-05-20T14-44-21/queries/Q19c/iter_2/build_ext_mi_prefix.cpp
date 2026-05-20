// Build a dict-encoded "prefix" column version for movie_info.info.
// Prefix = substring before the first ':' character (or empty if none).
// Output: column_versions/movie_info.info.prefix_dict/
//   codes.bin       : uint16_t[N_mi]    (1-based dict ID, 0 = "no ':' separator")
//   dict.offsets    : uint64_t[U+1]     (byte offsets into dict.data)
//   dict.data       : raw prefix bytes (no separators)
//
// Use case: query filters like `mi.info LIKE 'Japan:%...'` can resolve the
// prefix string -> dict id, then test code[r] == id with a single comparison
// before reading varlen mi.info bytes.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>

static void* map_ro(const std::string& path, size_t& out_size) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path.c_str(), strerror(errno)); exit(1); }
    struct stat st{};
    fstat(fd, &st);
    out_size = st.st_size;
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "mmap %s\n", path.c_str()); exit(1); }
    close(fd);
    return p;
}

static void write_all(const std::string& path, const void* data, size_t n) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "open %s for write: %s\n", path.c_str(), strerror(errno)); exit(1); }
    const char* p = (const char*)data;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { fprintf(stderr, "write %s\n", path.c_str()); exit(1); }
        p += w; n -= w;
    }
    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <gendb_dir>\n", argv[0]); return 1; }
    std::string gdir = argv[1];

    auto t0 = std::chrono::steady_clock::now();

    size_t off_size = 0, dat_size = 0;
    const uint8_t* off_raw = (const uint8_t*)map_ro(gdir + "/movie_info/info.off", off_size);
    const uint8_t* dat_raw = (const uint8_t*)map_ro(gdir + "/movie_info/info.dat", dat_size);
    const int64_t* off = (const int64_t*)off_raw;
    size_t N = off_size / 8 - 1;

    fprintf(stderr, "movie_info rows: %zu, info.dat: %zu bytes\n", N, dat_size);

    // Pass 1: collect unique prefixes
    std::unordered_map<std::string, uint16_t> dict;
    dict.reserve(512);
    std::vector<std::string> dict_strs;
    dict_strs.reserve(512);

    std::vector<uint16_t> codes(N, 0);

    for (size_t i = 0; i < N; i++) {
        size_t s = (size_t)off[i];
        size_t e = (size_t)off[i + 1];
        const uint8_t* p = dat_raw + s;
        size_t n = e - s;
        // Strict prefix: letters only, length 2..20, terminated by ':'.
        // Excludes quote-style prefixes ('Elwood P. Dowd:', 'U. S. ...:'), etc.
        size_t k = 0;
        bool valid = true;
        while (k < n && k < 20 && p[k] != ':') {
            uint8_t c = p[k];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            if (!ok) { valid = false; break; }
            k++;
        }
        if (!valid || k < 2 || k >= 20 || k == n || p[k] != ':') {
            codes[i] = 0;
            continue;
        }
        std::string pref((const char*)p, k);
        auto it = dict.find(pref);
        if (it == dict.end()) {
            uint16_t id = (uint16_t)(dict_strs.size() + 1); // 1-based
            if (id == 0) {
                fprintf(stderr, "too many prefixes (>65535) at row %zu; first 5 sample:\n", i);
                for (int j = 0; j < 5 && j < (int)dict_strs.size(); j++)
                    fprintf(stderr, "  %d: '%s'\n", j, dict_strs[dict_strs.size()-1-j].c_str());
                return 1;
            }
            dict[pref] = id;
            dict_strs.push_back(pref);
            codes[i] = id;
        } else {
            codes[i] = it->second;
        }
    }

    fprintf(stderr, "unique prefixes: %zu\n", dict_strs.size());

    // Compute dict.offsets and dict.data
    std::vector<uint64_t> doff(dict_strs.size() + 1, 0);
    size_t total = 0;
    for (size_t i = 0; i < dict_strs.size(); i++) {
        doff[i] = total;
        total += dict_strs[i].size();
    }
    doff[dict_strs.size()] = total;
    std::vector<uint8_t> ddata(total);
    for (size_t i = 0; i < dict_strs.size(); i++) {
        memcpy(ddata.data() + doff[i], dict_strs[i].data(), dict_strs[i].size());
    }

    std::string outdir = gdir + "/column_versions/movie_info.info.prefix_dict";
    {
        std::string mkdir_cmd = "mkdir -p " + outdir;
        if (system(mkdir_cmd.c_str()) != 0) { fprintf(stderr, "mkdir failed\n"); return 1; }
    }

    write_all(outdir + "/codes.bin", codes.data(), codes.size() * sizeof(uint16_t));
    write_all(outdir + "/dict.offsets", doff.data(), doff.size() * sizeof(uint64_t));
    write_all(outdir + "/dict.data", ddata.data(), ddata.size());

    auto t1 = std::chrono::steady_clock::now();
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    fprintf(stderr, "wrote %zu codes, %zu prefixes (%zu bytes data), %ld ms\n",
            codes.size(), dict_strs.size(), ddata.size(), ms);

    // Print verification summary
    printf("rows=%zu unique_prefixes=%zu build_ms=%ld\n", N, dict_strs.size(), ms);
    return 0;
}
