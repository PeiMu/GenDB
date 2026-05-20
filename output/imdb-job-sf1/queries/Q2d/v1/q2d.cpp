// Q2d: SELECT MIN(t.title) FROM cn, k, mc, mk, t
//      WHERE cn.country_code='[us]' AND k.keyword='character-name-in-title'
//        AND cn.id=mc.company_id AND mc.movie_id=t.id
//        AND t.id=mk.movie_id AND mk.keyword_id=k.id
//
// Strategy (from plan):
//   1. Resolve '[us]' -> target_code via cn dict.
//   2. Build cn_pass bitset over cn.id (dense identity).
//   3. Resolve 'character-name-in-title' -> target_k_id via keyword.dat scan.
//   4. Drive: mk CSR slice [off[k], off[k+1]) -> candidate movie_id mv.
//   5. For each mv: walk mc CSR slice; if cn_pass[mc.company_id[r]],
//      compare title.title[mv] against running min.

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <filesystem>

#include <omp.h>

using namespace gendb;

static inline std::string join_path(const std::string& a, const std::string& b) {
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string out_dir = argv[2];
    std::filesystem::create_directories(out_dir);

    // ---- Open column files ----
    MmapColumn<int16_t>  cn_cc;             // country_code codes
    MmapColumn<int64_t>  cn_dict_off;       // dict offsets
    MmapColumn<char>     cn_dict_dat;       // dict data
    MmapColumn<int64_t>  kw_off;            // keyword varlen offsets
    MmapColumn<char>     kw_dat;            // keyword varlen data
    MmapColumn<int32_t>  mk_off;            // movie_keyword__keyword_id offsets (CSR)
    MmapColumn<int32_t>  mk_rowids;         // movie_keyword__keyword_id rowids (CSR)
    MmapColumn<int32_t>  mk_movie_id;       // movie_keyword.movie_id
    MmapColumn<int32_t>  mc_off;            // movie_companies__movie_id offsets (CSR)
    MmapColumn<int32_t>  mc_company_id;     // movie_companies.company_id
    MmapColumn<int64_t>  t_title_off;       // title varlen offsets
    MmapColumn<char>     t_title_dat;       // title varlen data

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(join_path(store, "company_name/country_code.bin"));
        cn_dict_off.open(join_path(store, "company_name/country_code.dict.off"));
        cn_dict_dat.open(join_path(store, "company_name/country_code.dict.dat"));
        kw_off.open(join_path(store, "keyword/keyword.off"));
        kw_dat.open(join_path(store, "keyword/keyword.dat"));
        mk_off.open(join_path(store, "_idx/movie_keyword__keyword_id__offsets.bin"));
        mk_rowids.open(join_path(store, "_idx/movie_keyword__keyword_id__rowids.bin"));
        mk_movie_id.open(join_path(store, "movie_keyword/movie_id.bin"));
        mc_off.open(join_path(store, "_idx/movie_companies__movie_id__offsets.bin"));
        mc_company_id.open(join_path(store, "movie_companies/company_id.bin"));
        t_title_off.open(join_path(store, "title/title.off"));
        t_title_dat.open(join_path(store, "title/title.dat"));
        // mc_company_id will be probed randomly via mc rows of each candidate movie
        mc_company_id.advise_random();
        // title.title accessed only on surviving rows; random access
        t_title_off.advise_random();
        t_title_dat.advise_random();
    }

    // ---- Resolve '[us]' -> target_code by scanning country_code dict ----
    int16_t target_code = 0;
    {
        size_t nentries = cn_dict_off.count > 0 ? cn_dict_off.count - 1 : 0;
        for (size_t i = 0; i < nentries; ++i) {
            int64_t s = cn_dict_off[i];
            int64_t e = cn_dict_off[i + 1];
            std::string_view sv(cn_dict_dat.data + s, (size_t)(e - s));
            if (sv == "[us]") { target_code = (int16_t)(i + 1); break; }
        }
    }

    // ---- Build cn_pass bitset over cn.id (dense identity, 234997 rows) ----
    const size_t cn_rows = cn_cc.count;
    std::vector<uint64_t> cn_pass((cn_rows + 63) / 64, 0ULL);
    {
        GENDB_PHASE("build_cn_pass");
        const int16_t* cc = cn_cc.data;
        const int16_t tc = target_code;
        // Parallel set of bits
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < cn_rows; ++i) {
            if (cc[i] == tc) {
                __atomic_or_fetch(&cn_pass[i >> 6], (1ULL << (i & 63)), __ATOMIC_RELAXED);
            }
        }
    }

    // ---- Resolve 'character-name-in-title' -> target_k_id ----
    int32_t target_k_id = -1;
    {
        const std::string_view target = "character-name-in-title";
        size_t nentries = kw_off.count > 0 ? kw_off.count - 1 : 0;
        for (size_t i = 0; i < nentries; ++i) {
            int64_t s = kw_off[i];
            int64_t e = kw_off[i + 1];
            if ((size_t)(e - s) == target.size() &&
                std::memcmp(kw_dat.data + s, target.data(), target.size()) == 0) {
                target_k_id = (int32_t)i; // keyword.id is dense identity (id - 1 = row index? Let's check below)
                break;
            }
        }
    }

    // Note: For dense identity PKs in GenDB, the row index equals (id - 1) typically.
    // The CSR `movie_keyword__keyword_id__offsets.bin` indexes by keyword_id value
    // (so off has length max_keyword_id + 2 or similar). Per the guide:
    //   "for k_pos in [off[target_k_id], off[target_k_id+1])"
    // The example uses target_k_id directly. Since k.id starts at 1 and the file
    // ordering is by id, we need target_k_id = i + 1 (the actual id value, not row index).
    // Re-derive: the keyword's id value = row_index + 1 if PK is dense identity from 1.
    // The guide example explicitly uses `off[target_k_id]`, treating target_k_id as the
    // keyword id value. Let me use id = row_index + 1.

    int32_t target_k_id_val = (target_k_id >= 0) ? (target_k_id + 1) : -1;

    // But the offsets file may be indexed by row index instead of id value.
    // Check by file size: offsets.count should be ~134170 + 1 if indexed by row,
    // or up to max(keyword_id)+2 if by id. They're equivalent when id == row+1
    // and dense from 1.

    if (target_k_id < 0 || target_code == 0) {
        // No match — write empty result
        std::string out_path = join_path(out_dir, "Q2d.csv");
        FILE* f = std::fopen(out_path.c_str(), "w");
        std::fprintf(f, "movie_title\n");
        std::fclose(f);
        return 0;
    }

    // The CSR is indexed by keyword_id value. The guide's snippet uses target_k_id directly
    // (so the variable name target_k_id in the guide IS the id value). We have id = row+1.
    int32_t k_id_for_csr = target_k_id_val;

    // Sanity: clamp
    if (k_id_for_csr < 0 || (size_t)(k_id_for_csr + 1) >= mk_off.count) {
        std::string out_path = join_path(out_dir, "Q2d.csv");
        FILE* f = std::fopen(out_path.c_str(), "w");
        std::fprintf(f, "movie_title\n");
        std::fclose(f);
        return 0;
    }

    int32_t k_lo = mk_off[k_id_for_csr];
    int32_t k_hi = mk_off[k_id_for_csr + 1];

    // ---- Main scan: drive over mk CSR slice, probe mc per movie, check cn_pass ----
    const uint64_t* cn_pass_data = cn_pass.data();
    const int32_t* mk_rowids_p = mk_rowids.data;
    const int32_t* mk_movie_p  = mk_movie_id.data;
    const int32_t* mc_off_p    = mc_off.data;
    const int32_t* mc_cmpid_p  = mc_company_id.data;
    const int64_t* t_off_p     = t_title_off.data;
    const char*    t_dat_p     = t_title_dat.data;

    int32_t slice_n = k_hi - k_lo;
    int nthreads = std::max(1, (int)std::thread::hardware_concurrency());
    if (slice_n < nthreads) nthreads = std::max(1, slice_n);

    std::vector<std::string> local_best(nthreads);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel num_threads(nthreads)
        {
            int tid = omp_get_thread_num();
            std::string best;
            std::string_view best_sv;
            bool have = false;

            #pragma omp for schedule(static)
            for (int32_t k_pos = k_lo; k_pos < k_hi; ++k_pos) {
                int32_t mk_row = mk_rowids_p[k_pos];
                int32_t mv = mk_movie_p[mk_row];
                if (mv < 0) continue;

                int32_t mc_lo = mc_off_p[mv];
                int32_t mc_hi = mc_off_p[mv + 1];
                bool matched = false;
                for (int32_t r = mc_lo; r < mc_hi; ++r) {
                    int32_t cid = mc_cmpid_p[r];
                    // cn.id is dense identity from 1, but movie_companies.company_id stores
                    // the id value. The cn_pass bitset is indexed by row (0..cn_rows-1).
                    // Need: row = id - 1 typically. Test for safety.
                    uint32_t idx;
                    if (cid >= 1 && (size_t)cid <= cn_rows) {
                        idx = (uint32_t)(cid - 1);
                    } else if (cid >= 0 && (size_t)cid < cn_rows) {
                        idx = (uint32_t)cid;
                    } else {
                        continue;
                    }
                    if (cn_pass_data[idx >> 6] & (1ULL << (idx & 63))) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;

                // Compare title.title[mv]. title.off uses standard varlen layout
                // (size = title_rows + 1) so row index = id - 1; title for id=mv lives in
                // [t_off[mv-1], t_off[mv]).
                int64_t ts = t_off_p[mv - 1];
                int64_t te = t_off_p[mv];
                std::string_view sv(t_dat_p + ts, (size_t)(te - ts));
                if (!have || sv < best_sv) {
                    best.assign(sv.data(), sv.size());
                    best_sv = std::string_view(best);
                    have = true;
                }
            }

            if (have) local_best[tid] = std::move(best);
        }
    }

    // ---- Final reduction ----
    std::string global_best;
    bool have_global = false;
    for (auto& s : local_best) {
        if (s.empty()) continue;
        if (!have_global || s < global_best) {
            global_best = s;
            have_global = true;
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = join_path(out_dir, "Q2d.csv");
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "movie_title\n");
        if (have_global) {
            std::fwrite(global_best.data(), 1, global_best.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
