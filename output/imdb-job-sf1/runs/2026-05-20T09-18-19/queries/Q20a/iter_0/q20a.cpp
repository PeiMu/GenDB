// Q20a: complete_downey_ironman_movie
// SELECT MIN(t.title) FROM complete_cast, comp_cast_type x2, char_name, cast_info,
//   keyword, kind_type, movie_keyword, name, title
// with multiple LIKE filters.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "timing_utils.h"
#include "mmap_utils.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <fstream>
#include <atomic>
#include <thread>
#include <omp.h>

using gendb::MmapColumn;

// memmem-based contains
static inline bool contains(const char* s, size_t slen, const char* needle, size_t nlen) {
    if (nlen == 0) return true;
    if (slen < nlen) return false;
    return memmem(s, slen, needle, nlen) != nullptr;
}

// two-pointer LIKE %A%B% where A != B may overlap
static inline bool contains_pair(const char* s, size_t slen,
                                 const char* a, size_t alen,
                                 const char* b, size_t blen) {
    if (slen < alen + blen && !(alen == 0 || blen == 0)) {
        // need at least alen+blen chars (could overlap if alen+blen > slen and they share)
        // be safe: just fall back to two memmem
    }
    const char* p = (const char*)memmem(s, slen, a, alen);
    if (!p) return false;
    size_t remain_off = (p - s) + alen;
    if (remain_off > slen) return false;
    return memmem(s + remain_off, slen - remain_off, b, blen) != nullptr;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string out_dir = argv[2];

    // ------------------------------------------------------------------
    // Phase: data_loading — mmap all needed columns
    // ------------------------------------------------------------------
    MmapColumn<int64_t> cct_off;
    MmapColumn<char>    cct_dat;
    MmapColumn<int64_t> kt_off;
    MmapColumn<char>    kt_dat;
    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;
    MmapColumn<int64_t> chn_off;
    MmapColumn<char>    chn_dat;

    MmapColumn<int32_t> cc_mid, cc_subj, cc_stat;
    MmapColumn<int32_t> mk_mid, mk_kid;
    MmapColumn<int32_t> mkk_off_idx, mkk_row_idx;
    MmapColumn<int32_t> ci_off_idx;
    MmapColumn<int32_t> ci_prole;
    MmapColumn<int32_t> t_kind, t_year;
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;

    {
        GENDB_PHASE("data_loading");
        cct_off.open(store + "/comp_cast_type/kind.off");
        cct_dat.open(store + "/comp_cast_type/kind.dat");
        kt_off.open(store + "/kind_type/kind.off");
        kt_dat.open(store + "/kind_type/kind.dat");
        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");
        chn_off.open(store + "/char_name/name.off");
        chn_dat.open(store + "/char_name/name.dat");

        cc_mid.open(store + "/complete_cast/movie_id.bin");
        cc_subj.open(store + "/complete_cast/subject_id.bin");
        cc_stat.open(store + "/complete_cast/status_id.bin");

        mk_mid.open(store + "/movie_keyword/movie_id.bin");
        mk_kid.open(store + "/movie_keyword/keyword_id.bin");

        mkk_off_idx.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mkk_row_idx.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");

        ci_off_idx.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_prole.open(store + "/cast_info/person_role_id.bin");

        t_kind.open(store + "/title/kind_id.bin");
        t_year.open(store + "/title/production_year.bin");
        title_off.open(store + "/title/title.off");
        title_dat.open(store + "/title/title.dat");

        // prefetch the big columns we'll touch
        chn_dat.prefetch();
        chn_off.prefetch();
        cc_mid.prefetch();
        cc_subj.prefetch();
        cc_stat.prefetch();
    }

    // ------------------------------------------------------------------
    // Phase: dim_resolution
    // ------------------------------------------------------------------
    int32_t cct1_id = 0;
    std::vector<int32_t> cct2_ids;
    {
        GENDB_PHASE("resolve_dims");
        size_t n = cct_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            const char* s = cct_dat.data + cct_off[i];
            size_t len = cct_off[i+1] - cct_off[i];
            if (len == 4 && memcmp(s, "cast", 4) == 0) cct1_id = (int32_t)(i+1);
            if (memmem(s, len, "complete", 8)) cct2_ids.push_back((int32_t)(i+1));
        }
    }
    int32_t kt_movie = 0;
    {
        size_t n = kt_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            const char* s = kt_dat.data + kt_off[i];
            size_t len = kt_off[i+1] - kt_off[i];
            if (len == 5 && memcmp(s, "movie", 5) == 0) { kt_movie = (int32_t)(i+1); break; }
        }
    }
    // Resolve K_ids
    static const char* KW_LIT[] = {
        "superhero","sequel","second-part","marvel-comics",
        "based-on-comic","tv-special","fight","violence"
    };
    static const size_t KW_LEN[] = {9,6,11,13,14,10,5,8};
    std::vector<int32_t> K_ids;
    {
        size_t n = kw_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            size_t len = kw_off[i+1] - kw_off[i];
            const char* s = kw_dat.data + kw_off[i];
            for (int q = 0; q < 8; ++q) {
                if (len == KW_LEN[q] && memcmp(s, KW_LIT[q], len) == 0) {
                    K_ids.push_back((int32_t)(i+1));
                    break;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase: build_chn — parallel scan of char_name.name
    //   filter: NOT contains 'Sherlock' AND (Tony..Stark OR Iron..Man)
    // ------------------------------------------------------------------
    const size_t chn_n = chn_off.count - 1;
    std::vector<uint8_t> CHN(chn_n + 1, 0); // index by chn.id (1-based)
    {
        GENDB_PHASE("build_chn");
        const int64_t* offs = chn_off.data;
        const char* dat = chn_dat.data;
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < chn_n; ++i) {
            const char* s = dat + offs[i];
            size_t len = (size_t)(offs[i+1] - offs[i]);
            // NOT Sherlock
            if (contains(s, len, "Sherlock", 8)) continue;
            // (Tony Stark) OR (Iron Man)
            bool ok = contains_pair(s, len, "Tony", 4, "Stark", 5)
                   || contains_pair(s, len, "Iron", 4, "Man", 3);
            if (ok) {
                CHN[i+1] = 1;
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase: scan_cc — collect M_cc (movie_ids where subject==cct1 && status in cct2)
    //   complete_cast has 135K rows. Direct scan.
    //   M_cc may have duplicates; keep as a sorted unique vector OR a bitset over title space.
    // ------------------------------------------------------------------
    const size_t title_n = t_kind.count;
    std::vector<uint8_t> M_cc(title_n + 1, 0);
    {
        GENDB_PHASE("scan_cc");
        size_t n = cc_mid.count;
        const int32_t* mid = cc_mid.data;
        const int32_t* subj = cc_subj.data;
        const int32_t* stat = cc_stat.data;
        // cct2_ids is tiny; expand to scalar checks.
        int32_t c0 = cct2_ids.size() > 0 ? cct2_ids[0] : -1;
        int32_t c1 = cct2_ids.size() > 1 ? cct2_ids[1] : -1;
        int32_t c2 = cct2_ids.size() > 2 ? cct2_ids[2] : -1;
        int32_t c3 = cct2_ids.size() > 3 ? cct2_ids[3] : -1;
        for (size_t i = 0; i < n; ++i) {
            if (subj[i] != cct1_id) continue;
            int32_t st = stat[i];
            if (st == c0 || st == c1 || st == c2 || st == c3) {
                int32_t m = mid[i];
                if (m == INT32_MIN) continue;
                if ((size_t)m <= title_n) M_cc[m] = 1;
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase: build_kw — for each K_id, walk CSR slice and set M_kw bits
    // ------------------------------------------------------------------
    std::vector<uint8_t> M_kw(title_n + 1, 0);
    {
        GENDB_PHASE("build_kw");
        const int32_t* off = mkk_off_idx.data;
        const int32_t* row = mkk_row_idx.data;
        const int32_t* mid = mk_mid.data;
        for (int32_t kid : K_ids) {
            int32_t lo = off[kid];
            int32_t hi = off[kid+1];
            for (int32_t k2 = lo; k2 < hi; ++k2) {
                int32_t r = row[k2];
                int32_t m = mid[r];
                if (m != INT32_MIN && (size_t)m <= title_n) M_kw[m] = 1;
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase: main_scan — iterate candidate movies (M_cc ∩ M_kw),
    //   apply title filters, probe cast_info for CHN match
    // ------------------------------------------------------------------
    // Collect candidate movie_ids
    std::vector<int32_t> cand;
    cand.reserve(1024);
    for (size_t m = 1; m <= title_n; ++m) {
        if (M_cc[m] && M_kw[m]) {
            int32_t kid = t_kind.data[m-1];
            int32_t yr = t_year.data[m-1];
            if (kid == kt_movie && yr != INT32_MIN && yr > 1950) {
                cand.push_back((int32_t)m);
            }
        }
    }

    std::string best_title;
    bool best_set = false;
    {
        GENDB_PHASE("main_scan");
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* prole = ci_prole.data;
        // parallel over candidates
        std::vector<std::string> per_thread_best;
        std::vector<uint8_t> per_thread_set;
        int nthr = omp_get_max_threads();
        per_thread_best.resize(nthr);
        per_thread_set.assign(nthr, 0);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string local_best;
            bool local_set = false;
            #pragma omp for schedule(dynamic, 16) nowait
            for (size_t i = 0; i < cand.size(); ++i) {
                int32_t mv = cand[i];
                int32_t lo = ci_off[mv];
                int32_t hi = ci_off[mv+1];
                bool hit = false;
                for (int32_t r = lo; r < hi; ++r) {
                    int32_t prid = prole[r];
                    if (prid == INT32_MIN) continue;
                    if ((size_t)prid <= chn_n && CHN[prid]) { hit = true; break; }
                }
                if (!hit) continue;
                // fetch title
                int64_t tlo = title_off.data[mv-1];
                int64_t thi = title_off.data[mv];
                std::string_view tv(title_dat.data + tlo, (size_t)(thi - tlo));
                if (!local_set || tv < std::string_view(local_best)) {
                    local_best.assign(tv);
                    local_set = true;
                }
            }
            per_thread_best[tid] = std::move(local_best);
            per_thread_set[tid] = local_set ? 1 : 0;
        }
        for (int t = 0; t < nthr; ++t) {
            if (per_thread_set[t]) {
                if (!best_set || per_thread_best[t] < best_title) {
                    best_title = per_thread_best[t];
                    best_set = true;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase: output
    // ------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = out_dir + "/Q20a.csv";
        std::ofstream f(out_path);
        f << "complete_downey_ironman_movie\n";
        if (best_set) f << best_title << "\n";
    }
    return 0;
}
