// Q17b: MIN(n.name) where name LIKE 'Z%', keyword='character-name-in-title'
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <filesystem>
#include "timing_utils.h"
#include "cli_params.h"

namespace fs = std::filesystem;

struct Mapped {
    const void* ptr = nullptr;
    size_t bytes = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { std::fprintf(stderr, "stat failed: %s\n", path.c_str()); std::exit(1); }
    Mapped m;
    m.bytes = (size_t)st.st_size;
    if (m.bytes == 0) { m.ptr = nullptr; ::close(fd); return m; }
    void* p = ::mmap(nullptr, m.bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    m.ptr = p;
    ::close(fd);
    ::madvise(p, m.bytes, MADV_WILLNEED);
    return m;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // --- Data loading ---
    Mapped kw_off_m, kw_dat_m;
    Mapped mk_koff_m, mk_krow_m, mk_movieid_m;
    Mapped mc_off_m;
    Mapped ci_off_m, ci_pid_m;
    Mapped n_off_m, n_dat_m;

    {
        GENDB_PHASE("data_loading");
        kw_off_m   = map_file(store + "/keyword/keyword.off");
        kw_dat_m   = map_file(store + "/keyword/keyword.dat");
        mk_koff_m  = map_file(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_krow_m  = map_file(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movieid_m = map_file(store + "/movie_keyword/movie_id.bin");
        mc_off_m   = map_file(store + "/_idx/movie_companies__movie_id__offsets.bin");
        ci_off_m   = map_file(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_pid_m   = map_file(store + "/cast_info/person_id.bin");
        n_off_m    = map_file(store + "/name/name.off");
        n_dat_m    = map_file(store + "/name/name.dat");
    }

    const int64_t* kw_off    = (const int64_t*)kw_off_m.ptr;
    const char*    kw_dat    = (const char*)kw_dat_m.ptr;
    const int32_t* mk_koff   = (const int32_t*)mk_koff_m.ptr;
    const int32_t* mk_krow   = (const int32_t*)mk_krow_m.ptr;
    const int32_t* mk_movieid= (const int32_t*)mk_movieid_m.ptr;
    const int32_t* mc_off    = (const int32_t*)mc_off_m.ptr;
    const int32_t* ci_off    = (const int32_t*)ci_off_m.ptr;
    const int32_t* ci_pid    = (const int32_t*)ci_pid_m.ptr;
    const int64_t* n_off     = (const int64_t*)n_off_m.ptr;
    const char*    n_dat     = (const char*)n_dat_m.ptr;

    const int64_t n_rows = (int64_t)(n_off_m.bytes / sizeof(int64_t)) - 1;
    const int64_t kw_rows = (int64_t)(kw_off_m.bytes / sizeof(int64_t)) - 1;
    const int64_t mc_off_n = (int64_t)(mc_off_m.bytes / sizeof(int32_t));
    const int64_t ci_off_n = (int64_t)(ci_off_m.bytes / sizeof(int32_t));

    // --- Resolve keyword id ---
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        const char* target = "character-name-in-title";
        size_t tlen = std::strlen(target);
        for (int64_t i = 0; i < kw_rows; ++i) {
            int64_t a = kw_off[i], b = kw_off[i+1];
            if ((size_t)(b - a) == tlen && std::memcmp(kw_dat + a, target, tlen) == 0) {
                k_id = (int32_t)i;
                break;
            }
        }
        if (k_id < 0) { std::fprintf(stderr, "keyword not found\n"); return 1; }
    }

    // --- Build z_prefix bitset (name LIKE 'Z%') ---
    std::vector<uint64_t> z_prefix((n_rows + 63) / 64, 0);
    {
        GENDB_PHASE("build_z_prefix");
        unsigned T = std::max(1u, std::thread::hardware_concurrency());
        if (T > 12) T = 12;
        std::vector<std::thread> ths;
        for (unsigned t = 0; t < T; ++t) {
            ths.emplace_back([&, t]() {
                int64_t lo = (n_rows * t) / T;
                int64_t hi = (n_rows * (t+1)) / T;
                for (int64_t i = lo; i < hi; ++i) {
                    int64_t a = n_off[i], b = n_off[i+1];
                    if (b > a && n_dat[a] == 'Z') {
                        // atomic-friendly: each thread writes to disjoint range
                        // but bitset bits can share words at boundaries.
                        __sync_fetch_and_or(&z_prefix[i >> 6], (uint64_t)1 << (i & 63));
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
    }

    // --- Main scan: CSR over mk[k_id], probe mc existence, scan ci, check z_prefix ---
    // Offsets are 1-indexed: file has N+2 entries; offsets[i]..offsets[i+1] for 1-based id i.
    int32_t mk_lo = mk_koff[k_id + 1];
    int32_t mk_hi = mk_koff[k_id + 2];
    int32_t mk_count = mk_hi - mk_lo;

    // Per-thread best result. Track best as (off, len) pointing into n.dat.
    struct Best {
        int64_t off = -1;
        int32_t len = 0;
    };

    unsigned T = std::max(1u, std::thread::hardware_concurrency());
    if (T > 12) T = 12;
    if ((int32_t)T > mk_count) T = std::max(1, mk_count);
    std::vector<Best> bests(T);

    auto cmp_better = [&](int64_t a_off, int32_t a_len, int64_t b_off, int32_t b_len) {
        // returns true if (a_off,a_len) < (b_off,b_len) lexicographically
        int32_t mn = a_len < b_len ? a_len : b_len;
        int c = std::memcmp(n_dat + a_off, n_dat + b_off, (size_t)mn);
        if (c != 0) return c < 0;
        return a_len < b_len;
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> ths;
        for (unsigned t = 0; t < T; ++t) {
            ths.emplace_back([&, t]() {
                int32_t lo = mk_lo + (int32_t)((int64_t)mk_count * t / T);
                int32_t hi = mk_lo + (int32_t)((int64_t)mk_count * (t+1) / T);
                Best local;
                for (int32_t k = lo; k < hi; ++k) {
                    int32_t mk_row = mk_krow[k];
                    int32_t mv = mk_movieid[mk_row]; // movie_id (1-based)
                    if (mv < 1 || (int64_t)mv + 1 >= mc_off_n) continue;
                    // mc existence probe: any mc row with movie_id=mv?
                    if (mc_off[mv + 1] <= mc_off[mv]) continue;
                    // cast_info rows for movie_id=mv
                    if ((int64_t)mv + 1 >= ci_off_n) continue;
                    int32_t ci_a = ci_off[mv];
                    int32_t ci_b = ci_off[mv + 1];
                    for (int32_t r = ci_a; r < ci_b; ++r) {
                        int32_t pid = ci_pid[r]; // person_id (1-based)
                        int32_t nidx = pid - 1;
                        if (nidx < 0 || nidx >= n_rows) continue;
                        if ((z_prefix[nidx >> 6] >> (nidx & 63)) & 1ULL) {
                            int64_t a = n_off[nidx], b = n_off[nidx + 1];
                            int32_t len = (int32_t)(b - a);
                            if (local.off < 0 || cmp_better(a, len, local.off, local.len)) {
                                local.off = a;
                                local.len = len;
                            }
                        }
                    }
                }
                bests[t] = local;
            });
        }
        for (auto& th : ths) th.join();
    }

    Best global;
    for (auto& b : bests) {
        if (b.off < 0) continue;
        if (global.off < 0 || cmp_better(b.off, b.len, global.off, global.len)) {
            global = b;
        }
    }

    // --- Output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q17b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "member_in_charnamed_movie,a1\n");
        if (global.off < 0) {
            std::fprintf(f, ",\n");
        } else {
            std::string s(n_dat + global.off, (size_t)global.len);
            // CSV: quote if contains comma or quote
            auto emit = [&](const std::string& v) {
                bool need_quote = false;
                for (char c : v) if (c == ',' || c == '"' || c == '\n') { need_quote = true; break; }
                if (!need_quote) { std::fputs(v.c_str(), f); return; }
                std::fputc('"', f);
                for (char c : v) {
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            };
            emit(s);
            std::fputc(',', f);
            emit(s);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
