// Q17f: SELECT MIN(n.name) AS member_in_charnamed_movie
// Driver: movie_keyword CSR keyed by k_id('character-name-in-title')
// Per movie: gate on mc existence; iterate ci rows; gate name on hasB; update MIN(name)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <climits>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    // Ensure results_dir exists
    {
        std::string mk = "mkdir -p '" + results_dir + "'";
        (void)system(mk.c_str());
    }

    // ---------- Data Loading ----------
    MmapColumn<char>     keyword_dat;
    MmapColumn<int64_t>  keyword_off;
    MmapColumn<char>     name_dat;
    MmapColumn<int64_t>  name_off;
    MmapColumn<int32_t>  mk_k_off;
    MmapColumn<int32_t>  mk_k_row;
    MmapColumn<int32_t>  mk_movie_id;
    MmapColumn<int32_t>  mc_off;
    MmapColumn<int32_t>  ci_off;
    MmapColumn<int32_t>  ci_person_id;

    {
        GENDB_PHASE("data_loading");
        keyword_dat.open(store + "/keyword/keyword.dat");
        keyword_off.open(store + "/keyword/keyword.off");
        name_dat.open(store + "/name/name.dat");
        name_off.open(store + "/name/name.off");
        mk_k_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_k_row.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");
        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        ci_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person_id.open(store + "/cast_info/person_id.bin");

        // Hints: random for person_id (lookups via ci offsets)
        ci_person_id.advise_random();
        // name.off and name.dat will be touched both linearly (hasB) and randomly (compares)
        mmap_prefetch_all(keyword_dat, keyword_off, name_dat, name_off,
                          mk_k_off, mk_k_row, mk_movie_id,
                          mc_off, ci_off, ci_person_id);
    }

    // ---------- Resolve keyword id ----------
    const char* kw_target = "character-name-in-title";
    size_t kw_target_len = std::strlen(kw_target);
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        size_t n_kw = keyword_off.count - 1;
        for (size_t i = 0; i < n_kw; ++i) {
            int64_t a = keyword_off[i];
            int64_t b = keyword_off[i+1];
            size_t len = (size_t)(b - a);
            if (len == kw_target_len &&
                std::memcmp(keyword_dat.data + a, kw_target, len) == 0) {
                // keyword.id is 1-based; CSR offsets are indexed by keyword.id
                k_id = (int32_t)(i + 1);
                break;
            }
        }
        if (k_id < 0) {
            std::fprintf(stderr, "keyword not found\n");
            return 2;
        }
    }

    // ---------- Pre-scan: build hasB ----------
    size_t n_names = name_off.count - 1;
    std::vector<uint8_t> hasB(n_names, 0);
    {
        GENDB_PHASE("prescan_name_hasB");
        // Parallel prescan
        unsigned T = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        size_t chunk = (n_names + T - 1) / T;
        for (unsigned t = 0; t < T; ++t) {
            size_t lo = t * chunk;
            size_t hi = std::min(n_names, lo + chunk);
            if (lo >= hi) continue;
            threads.emplace_back([&, lo, hi]() {
                for (size_t i = lo; i < hi; ++i) {
                    int64_t a = name_off[i];
                    int64_t b = name_off[i+1];
                    size_t n = (size_t)(b - a);
                    if (n > 0 && std::memchr(name_dat.data + a, 'B', n)) {
                        hasB[i] = 1;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // ---------- Get mk slice and dedup movie_ids ----------
    std::vector<int32_t> movies;
    {
        GENDB_PHASE("mk_slice_dedup");
        int32_t lo = mk_k_off[k_id];
        int32_t hi = mk_k_off[k_id + 1];
        movies.reserve((size_t)(hi - lo));
        for (int32_t k = lo; k < hi; ++k) {
            int32_t mk_row = mk_k_row[k];
            movies.push_back(mk_movie_id[mk_row]);
        }
        std::sort(movies.begin(), movies.end());
        movies.erase(std::unique(movies.begin(), movies.end()), movies.end());
    }

    // Helper: compare two names by person_id (1-based)
    auto name_lt = [&](int32_t pa, int32_t pb) -> bool {
        int64_t a0 = name_off[pa - 1], a1 = name_off[pa];
        int64_t b0 = name_off[pb - 1], b1 = name_off[pb];
        size_t la = (size_t)(a1 - a0);
        size_t lb = (size_t)(b1 - b0);
        size_t m = std::min(la, lb);
        int c = std::memcmp(name_dat.data + a0, name_dat.data + b0, m);
        if (c != 0) return c < 0;
        return la < lb;
    };

    // ---------- Main scan: parallel over deduped movies ----------
    int32_t best_pid = -1;
    {
        GENDB_PHASE("main_scan");
        size_t M = movies.size();
        unsigned T = std::max(1u, std::thread::hardware_concurrency());
        if (T > M) T = (unsigned)std::max<size_t>(1, M);

        std::vector<int32_t> local_best(T, -1);
        std::vector<std::thread> threads;
        size_t chunk = (M + T - 1) / T;

        for (unsigned t = 0; t < T; ++t) {
            size_t lo = t * chunk;
            size_t hi = std::min(M, lo + chunk);
            if (lo >= hi) continue;
            threads.emplace_back([&, t, lo, hi]() {
                int32_t lbest = -1;
                const int32_t* mc_offs = mc_off.data;
                const int32_t* ci_offs = ci_off.data;
                size_t mc_n = mc_off.count - 1;
                size_t ci_n = ci_off.count - 1;
                for (size_t i = lo; i < hi; ++i) {
                    int32_t mv = movies[i];
                    // mc existence: mc range non-empty for mv
                    if (mv < 0 || (size_t)mv >= mc_n) continue;
                    int32_t mc_lo = mc_offs[mv];
                    int32_t mc_hi = mc_offs[mv + 1];
                    if (mc_lo == mc_hi) continue;
                    // ci range for mv
                    if ((size_t)mv >= ci_n) continue;
                    int32_t ci_lo = ci_offs[mv];
                    int32_t ci_hi = ci_offs[mv + 1];
                    for (int32_t r = ci_lo; r < ci_hi; ++r) {
                        int32_t pid = ci_person_id[r];
                        if (pid <= 0 || (size_t)(pid - 1) >= n_names) continue;
                        if (!hasB[pid - 1]) continue;
                        if (lbest < 0 || name_lt(pid, lbest)) {
                            lbest = pid;
                        }
                    }
                }
                local_best[t] = lbest;
            });
        }
        for (auto& th : threads) th.join();

        for (unsigned t = 0; t < T; ++t) {
            int32_t lb = local_best[t];
            if (lb < 0) continue;
            if (best_pid < 0 || name_lt(lb, best_pid)) {
                best_pid = lb;
            }
        }
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q17f.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "member_in_charnamed_movie\n");
        if (best_pid > 0) {
            int64_t a = name_off[best_pid - 1];
            int64_t b = name_off[best_pid];
            const char* p = name_dat.data + a;
            size_t n = (size_t)(b - a);
            // CSV escape: if contains comma, quote, or newline -> wrap in quotes and double internal quotes
            bool need_quote = false;
            for (size_t i = 0; i < n; ++i) {
                char c = p[i];
                if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                    need_quote = true;
                    break;
                }
            }
            if (need_quote) {
                std::fputc('"', f);
                for (size_t i = 0; i < n; ++i) {
                    char c = p[i];
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            } else {
                std::fwrite(p, 1, n, f);
            }
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
