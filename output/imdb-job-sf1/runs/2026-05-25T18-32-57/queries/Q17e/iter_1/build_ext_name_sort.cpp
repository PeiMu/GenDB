// Build storage extension: name__sort_by_name
// Produces an int32[N_name] array of row indices in lexicographic order of name.name.
// Empty/NULL names are placed at the end so the first non-empty match wins MIN.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::pair<void*, size_t> mmap_ro(const std::string& p) {
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(p.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(fd, &st) < 0) { std::perror("fstat"); std::exit(1); }
    void* m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
    close(fd);
    return {m, (size_t)st.st_size};
}

int main(int argc, char* argv[]) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <gendb_dir>\n", argv[0]); return 1; }
    auto t0 = std::chrono::high_resolution_clock::now();
    std::string gendb_dir = argv[1];

    auto [off_p, off_sz] = mmap_ro(gendb_dir + "/name/name.off");
    auto [dat_p, dat_sz] = mmap_ro(gendb_dir + "/name/name.dat");
    const uint64_t* off = (const uint64_t*)off_p;
    const char* dat = (const char*)dat_p;
    uint64_t N = (off_sz / 8) - 1;
    std::fprintf(stderr, "N_name=%lu, dat=%lu bytes\n", N, dat_sz);

    // Count non-empty for stats
    uint64_t non_empty = 0;
    for (uint64_t i = 0; i < N; ++i) if (off[i+1] > off[i]) ++non_empty;
    std::fprintf(stderr, "non_empty=%lu\n", non_empty);

    std::vector<int32_t> order(N);
    for (uint64_t i = 0; i < N; ++i) order[i] = (int32_t)i;

    // Sort: non-empty in lex order first; empty rows at the end.
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        uint64_t la = off[a+1] - off[a];
        uint64_t lb = off[b+1] - off[b];
        if (la == 0 && lb == 0) return a < b;
        if (la == 0) return false; // a is empty, goes after
        if (lb == 0) return true;  // b is empty, a comes first
        size_t cl = la < lb ? la : lb;
        int c = std::memcmp(dat + off[a], dat + off[b], cl);
        if (c != 0) return c < 0;
        return la < lb;
    });

    auto t_sort = std::chrono::high_resolution_clock::now();
    std::fprintf(stderr, "sorted in %ld ms\n",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t_sort - t0).count());

    // Sanity check first 3 names
    for (int i = 0; i < 3 && i < (int)N; ++i) {
        int32_t r = order[i];
        std::string_view s(dat + off[r], off[r+1] - off[r]);
        std::fprintf(stderr, "order[%d]=row %d name=\"%.*s\"\n",
                     i, r, (int)std::min<size_t>(s.size(), 60), s.data());
    }

    std::string out_dir = gendb_dir + "/column_versions/name.name.sortorder";
    std::filesystem::create_directories(out_dir);
    std::string out_path = out_dir + "/order.bin";
    FILE* fp = std::fopen(out_path.c_str(), "wb");
    if (!fp) { std::perror(out_path.c_str()); return 1; }
    size_t wrote = std::fwrite(order.data(), sizeof(int32_t), N, fp);
    std::fclose(fp);
    if (wrote != N) { std::fprintf(stderr, "short write\n"); return 1; }

    auto t1 = std::chrono::high_resolution_clock::now();
    std::fprintf(stderr, "wrote %s (%lu int32 rows) in %ld ms total\n",
                 out_path.c_str(), N,
                 std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return 0;
}
