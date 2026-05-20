// Q17d: SELECT MIN(n.name) ...
// Driver: keyword 'character-name-in-title' → movie_keyword CSR → mc existence
//                                            → cast_info person_ids → bert bitmap → MIN(name)

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <omp.h>

#include "timing_utils.h"

template <typename T>
struct MM {
    const T* data = nullptr;
    size_t n = 0;
    void* raw = nullptr;
    size_t raw_size = 0;
    int fd = -1;

    void open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
        struct stat st;
        fstat(fd, &st);
        raw_size = (size_t)st.st_size;
        raw = mmap(nullptr, raw_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (raw == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
        data = (const T*)raw;
        n = raw_size / sizeof(T);
    }
};

static inline bool name_lt(const MM<int64_t>& off, const MM<char>& dat,
                           int32_t a, int32_t b) {
    int64_t aa = off.data[a], ab = off.data[a + 1];
    int64_t ba = off.data[b], bb = off.data[b + 1];
    size_t la = (size_t)(ab - aa);
    size_t lb = (size_t)(bb - ba);
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(dat.data + aa, dat.data + ba, m);
    if (c != 0) return c < 0;
    return la < lb;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    MM<int64_t> kw_off;
    MM<char>    kw_dat;
    MM<int64_t> nm_off;
    MM<char>    nm_dat;
    MM<int32_t> mk_k_off;
    MM<int32_t> mk_k_row;
    MM<int32_t> mk_mv;
    MM<int32_t> mc_off;
    MM<int32_t> ci_off;
    MM<int32_t> ci_person;

    int64_t name_rows = 0;
    {
        GENDB_PHASE("data_loading");
        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");
        nm_off.open(store + "/name/name.off");
        nm_dat.open(store + "/name/name.dat");
        mk_k_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_k_row.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_mv.open(store + "/movie_keyword/movie_id.bin");
        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        ci_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person.open(store + "/cast_info/person_id.bin");
        name_rows = (int64_t)nm_off.n - 1;
    }

    // --- Resolve keyword id (linear scan over keyword.dat) ---
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        const char* target = "character-name-in-title";
        size_t tlen = std::strlen(target);
        int64_t kn = (int64_t)kw_off.n - 1;
        for (int64_t i = 0; i < kn; ++i) {
            int64_t a = kw_off.data[i], b = kw_off.data[i + 1];
            if ((size_t)(b - a) == tlen && std::memcmp(kw_dat.data + a, target, tlen) == 0) {
                k_id = (int32_t)i;
                break;
            }
        }
        if (k_id < 0) {
            std::fprintf(stderr, "keyword 'character-name-in-title' not found\n");
            return 1;
        }
    }
    // k_id from offsets above is 0-based row index of keyword table.
    // The CSR offsets file is indexed by keyword.id (1-based dense PK), so we
    // need k_id+1 as the lookup. Verify: mk_k_off.n - 1 should equal max keyword.id.
    int32_t k_lookup = k_id + 1;

    // --- Pre-scan name.name for 'Bert' substring → dense bitmap ---
    std::vector<uint8_t> bert;
    {
        GENDB_PHASE("prescan_bert");
        bert.assign((size_t)name_rows, 0);
        #pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < name_rows; ++i) {
            int64_t a = nm_off.data[i], b = nm_off.data[i + 1];
            size_t len = (size_t)(b - a);
            if (len >= 4 && memmem(nm_dat.data + a, len, "Bert", 4) != nullptr) {
                bert[(size_t)i] = 1;
            }
        }
    }

    // --- Main scan: iterate CSR slice for k_id; probe mc existence & ci ---
    int32_t best_id = -1;
    {
        GENDB_PHASE("main_scan");

        // The mk CSR offsets file has length 134172 = 134170 (rows) + 2.
        // Try k_lookup=k_id+1 (1-based keyword.id) — fall back to k_id if size hints otherwise.
        int32_t lo = 0, hi = 0;
        if ((size_t)k_lookup + 1 < mk_k_off.n) {
            lo = mk_k_off.data[k_lookup];
            hi = mk_k_off.data[k_lookup + 1];
        }
        // If empty, try 0-based fallback.
        if (hi <= lo) {
            lo = mk_k_off.data[k_id];
            hi = mk_k_off.data[k_id + 1];
        }

        int nthreads = omp_get_max_threads();
        std::vector<int32_t> local_best(nthreads, -1);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            int32_t lb = -1;

            #pragma omp for schedule(static)
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = mk_k_row.data[k];
                int32_t mv = mk_mv.data[r];
                if (mv <= 0) continue;
                // movie_companies existence
                if ((size_t)mv + 1 >= mc_off.n) continue;
                if (mc_off.data[mv + 1] - mc_off.data[mv] <= 0) continue;
                // cast_info slice
                if ((size_t)mv + 1 >= ci_off.n) continue;
                int32_t cl = ci_off.data[mv];
                int32_t ch = ci_off.data[mv + 1];
                for (int32_t c = cl; c < ch; ++c) {
                    int32_t pid = ci_person.data[c]; // 1-based name.id
                    int32_t idx = pid - 1;
                    if (idx < 0 || idx >= (int32_t)name_rows) continue;
                    if (!bert[(size_t)idx]) continue;
                    if (lb < 0 || name_lt(nm_off, nm_dat, idx, lb)) lb = idx;
                }
            }
            local_best[tid] = lb;
        }

        for (int t = 0; t < nthreads; ++t) {
            int32_t lb = local_best[t];
            if (lb < 0) continue;
            if (best_id < 0 || name_lt(nm_off, nm_dat, lb, best_id)) best_id = lb;
        }
    }

    // --- Output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q17d.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fputs("member_in_charnamed_movie\n", f);
        if (best_id >= 0) {
            int64_t a = nm_off.data[best_id], b = nm_off.data[best_id + 1];
            size_t len = (size_t)(b - a);
            const char* s = nm_dat.data + a;
            // Quote if contains comma or quote
            bool needs_quote = false;
            for (size_t i = 0; i < len; ++i) {
                if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') { needs_quote = true; break; }
            }
            if (needs_quote) {
                std::fputc('"', f);
                for (size_t i = 0; i < len; ++i) {
                    if (s[i] == '"') std::fputc('"', f);
                    std::fputc(s[i], f);
                }
                std::fputc('"', f);
            } else {
                std::fwrite(s, 1, len, f);
            }
            std::fputc('\n', f);
        } else {
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
