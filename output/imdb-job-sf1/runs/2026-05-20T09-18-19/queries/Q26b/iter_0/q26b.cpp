// Q26b — IMDB JOB
// SELECT MIN(chn.name), MIN(mi_idx.info), MIN(t.title) ...
// Pipeline: dim filters -> title driver (year>2005, kind=movie) ->
//   movie_keyword(kw_set) -> complete_cast(cct1, cct2_set) ->
//   movie_info_idx(it2, info>'8.0') -> cast_info(chn_set bitset).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <omp.h>

#include "mmap_utils.h"
#include "timing_utils.h"

using namespace gendb;

static constexpr int32_t NULL_INT = INT32_MIN;

static inline std::string_view sv_at(const char* dat, const int64_t* off, size_t i) {
    int64_t s = off[i];
    int64_t e = off[i + 1];
    return std::string_view(dat + s, static_cast<size_t>(e - s));
}

// Find first occurrence (returns -1 if not found) for the small dim scans.
static int32_t find_dim_eq(const char* dat, const int64_t* off, size_t n,
                           std::string_view target) {
    for (size_t i = 0; i < n; i++) {
        std::string_view s = sv_at(dat, off, i);
        if (s == target) return static_cast<int32_t>(i + 1);  // dense id = i+1
    }
    return -1;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];

    // -------------------- mmap all columns --------------------
    MmapColumn<char>    cct_dat, kt_dat, it_dat, kw_dat, chn_dat, ti_dat, mi_info_dat;
    MmapColumn<int64_t> cct_off, kt_off, it_off, kw_off, chn_off, ti_off, mi_info_off;
    MmapColumn<int32_t> t_kindid, t_year;
    MmapColumn<int32_t> mk_kid;                 // movie_keyword.keyword_id
    MmapColumn<int32_t> cc_subj, cc_status;     // complete_cast
    MmapColumn<int32_t> mi_itid;                // movie_info_idx.info_type_id
    MmapColumn<int32_t> ci_prole;               // cast_info.person_role_id
    MmapColumn<int32_t> off_mk, off_cc, off_mi, off_ci;  // per-movie offsets indexes

    {
        GENDB_PHASE("data_loading");

        cct_dat.open(gdir + "/comp_cast_type/kind.dat");
        cct_off.open(gdir + "/comp_cast_type/kind.off");
        kt_dat.open(gdir + "/kind_type/kind.dat");
        kt_off.open(gdir + "/kind_type/kind.off");
        it_dat.open(gdir + "/info_type/info.dat");
        it_off.open(gdir + "/info_type/info.off");
        kw_dat.open(gdir + "/keyword/keyword.dat");
        kw_off.open(gdir + "/keyword/keyword.off");
        chn_dat.open(gdir + "/char_name/name.dat");
        chn_off.open(gdir + "/char_name/name.off");
        ti_dat.open(gdir + "/title/title.dat");
        ti_off.open(gdir + "/title/title.off");
        mi_info_dat.open(gdir + "/movie_info_idx/info.dat");
        mi_info_off.open(gdir + "/movie_info_idx/info.off");

        t_kindid.open(gdir + "/title/kind_id.bin");
        t_year.open(gdir + "/title/production_year.bin");
        mk_kid.open(gdir + "/movie_keyword/keyword_id.bin");
        cc_subj.open(gdir + "/complete_cast/subject_id.bin");
        cc_status.open(gdir + "/complete_cast/status_id.bin");
        mi_itid.open(gdir + "/movie_info_idx/info_type_id.bin");
        ci_prole.open(gdir + "/cast_info/person_role_id.bin");

        off_mk.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        off_cc.open(gdir + "/_idx/complete_cast__movie_id__offsets.bin");
        off_mi.open(gdir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        off_ci.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");

        mmap_prefetch_all(t_kindid, t_year, off_mk, off_cc, off_mi, off_ci);
        mmap_prefetch_all(mk_kid, cc_subj, cc_status, mi_itid, ci_prole);
    }

    // -------------------- build dim sets --------------------
    int32_t cct1_id = -1;            // 'cast'
    int32_t cct2_set[4] = {-1, -1, -1, -1};
    int     cct2_n = 0;
    int32_t it2_id = -1;             // 'rating'
    int32_t movie_kind_id = -1;
    int32_t kw_set[4] = {-1, -1, -1, -1};
    int     kw_n = 0;
    std::vector<uint64_t> chn_set;
    size_t  chn_count = chn_off.count - 1;  // # char_name rows

    {
        GENDB_PHASE("dim_filter");

        // comp_cast_type
        for (size_t i = 0; i < cct_off.count - 1; i++) {
            std::string_view s = sv_at(cct_dat.data, cct_off.data, i);
            int32_t id = static_cast<int32_t>(i + 1);
            if (s == "cast") cct1_id = id;
            if (s.find("complete") != std::string_view::npos) {
                if (cct2_n < 4) cct2_set[cct2_n++] = id;
            }
        }

        // info_type 'rating'
        it2_id = find_dim_eq(it_dat.data, it_off.data, it_off.count - 1, "rating");
        // kind_type 'movie'
        movie_kind_id = find_dim_eq(kt_dat.data, kt_off.data, kt_off.count - 1, "movie");

        // keyword IN (4 vals)
        static const char* kws[4] = {"superhero", "marvel-comics", "based-on-comic", "fight"};
        for (size_t i = 0; i < kw_off.count - 1 && kw_n < 4; i++) {
            std::string_view s = sv_at(kw_dat.data, kw_off.data, i);
            for (int j = 0; j < 4; j++) {
                if (s == kws[j]) { kw_set[kw_n++] = static_cast<int32_t>(i + 1); break; }
            }
        }

        // char_name bitset: name NOT NULL AND (name LIKE '%man%' OR '%Man%')
        size_t n_words = (chn_count + 64 + 63) / 64;
        chn_set.assign(n_words, 0);

        const char* cd = chn_dat.data;
        const int64_t* co = chn_off.data;
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < chn_count; i++) {
            int64_t s = co[i], e = co[i + 1];
            size_t len = static_cast<size_t>(e - s);
            if (len == 0) continue;
            const char* p = cd + s;
            // memmem for "man" and "Man"
            bool hit = false;
            if (len >= 3) {
                if (memmem(p, len, "man", 3) || memmem(p, len, "Man", 3)) hit = true;
            }
            if (hit) {
                size_t id = i + 1;
                size_t w = id >> 6;
                uint64_t mask = 1ULL << (id & 63);
                #pragma omp atomic
                chn_set[w] |= mask;
            }
        }

        std::printf("[INFO] cct1_id=%d cct2_n=%d it2_id=%d kind_id=%d kw_n=%d\n",
                    cct1_id, cct2_n, it2_id, movie_kind_id, kw_n);
        if (cct1_id < 0 || cct2_n == 0 || it2_id < 0 || movie_kind_id < 0 || kw_n == 0) {
            std::fprintf(stderr, "FATAL: dim filter resolution failed\n");
            return 2;
        }
    }

    // -------------------- main scan: parallel over title ids --------------------
    // title id is dense 1..N. t_idx = t_id - 1.
    const size_t T_N = t_kindid.count;

    // Per-thread mins
    int nthreads = omp_get_max_threads();
    std::vector<std::string> tmin_chn(nthreads);   // empty == none
    std::vector<std::string> tmin_info(nthreads);
    std::vector<std::string> tmin_title(nthreads);
    std::vector<uint8_t> have_chn(nthreads, 0), have_info(nthreads, 0), have_title(nthreads, 0);

    {
        GENDB_PHASE("main_scan");

        const int32_t* T_kind = t_kindid.data;
        const int32_t* T_year = t_year.data;
        const int32_t* OF_mk = off_mk.data;
        const int32_t* OF_cc = off_cc.data;
        const int32_t* OF_mi = off_mi.data;
        const int32_t* OF_ci = off_ci.data;
        const int32_t* MK_kid = mk_kid.data;
        const int32_t* CC_subj = cc_subj.data;
        const int32_t* CC_stat = cc_status.data;
        const int32_t* MI_itid = mi_itid.data;
        const int32_t* CI_prole = ci_prole.data;

        const char*    CHN_dat = chn_dat.data;
        const int64_t* CHN_off = chn_off.data;
        const char*    MI_dat = mi_info_dat.data;
        const int64_t* MI_off = mi_info_off.data;
        const char*    TI_dat = ti_dat.data;
        const int64_t* TI_off = ti_off.data;

        const uint64_t* CHN_BIT = chn_set.data();
        const size_t CHN_COUNT = chn_count;

        const int32_t KW0 = kw_set[0], KW1 = kw_set[1], KW2 = kw_set[2], KW3 = kw_set[3];
        const int32_t IT2 = it2_id;
        const int32_t CCT1 = cct1_id;
        const int32_t CC2A = cct2_set[0];
        const int32_t CC2B = (cct2_n > 1) ? cct2_set[1] : -1;
        const int32_t CC2C = (cct2_n > 2) ? cct2_set[2] : -1;
        const int32_t CC2D = (cct2_n > 3) ? cct2_set[3] : -1;
        const int32_t MKIND = movie_kind_id;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string_view loc_chn, loc_info, loc_title;
            bool h_chn = false, h_info = false, h_title = false;

            #pragma omp for schedule(dynamic, 65536) nowait
            for (size_t t_idx = 0; t_idx < T_N; t_idx++) {
                // title filter
                int32_t y = T_year[t_idx];
                if (y == NULL_INT || y <= 2005) continue;
                if (T_kind[t_idx] != MKIND) continue;

                int32_t t_id = static_cast<int32_t>(t_idx + 1);

                // ---- movie_keyword: any keyword_id in kw_set?
                int32_t mk_lo = OF_mk[t_id], mk_hi = OF_mk[t_id + 1];
                bool kw_hit = false;
                for (int32_t r = mk_lo; r < mk_hi; r++) {
                    int32_t k = MK_kid[r];
                    if (k == KW0 || k == KW1 || k == KW2 || k == KW3) { kw_hit = true; break; }
                }
                if (!kw_hit) continue;

                // ---- complete_cast: subject_id == cct1 AND status_id in cct2_set
                int32_t cc_lo = OF_cc[t_id], cc_hi = OF_cc[t_id + 1];
                bool cc_hit = false;
                for (int32_t r = cc_lo; r < cc_hi; r++) {
                    if (CC_subj[r] != CCT1) continue;
                    int32_t st = CC_stat[r];
                    if (st == CC2A || st == CC2B || st == CC2C || st == CC2D) {
                        cc_hit = true; break;
                    }
                }
                if (!cc_hit) continue;

                // ---- movie_info_idx: info_type_id == it2 AND info > "8.0"
                int32_t mi_lo = OF_mi[t_id], mi_hi = OF_mi[t_id + 1];
                std::string_view local_min_info;
                bool have_local_info = false;
                for (int32_t r = mi_lo; r < mi_hi; r++) {
                    if (MI_itid[r] != IT2) continue;
                    std::string_view info = sv_at(MI_dat, MI_off, static_cast<size_t>(r));
                    if (!(info > std::string_view("8.0"))) continue;
                    if (!have_local_info || info < local_min_info) {
                        local_min_info = info;
                        have_local_info = true;
                    }
                }
                if (!have_local_info) continue;

                // ---- cast_info: person_role_id non-null AND in chn_set
                int32_t ci_lo = OF_ci[t_id], ci_hi = OF_ci[t_id + 1];
                std::string_view local_min_chn;
                bool have_local_chn = false;
                for (int32_t r = ci_lo; r < ci_hi; r++) {
                    int32_t pr = CI_prole[r];
                    if (pr == NULL_INT) continue;
                    if (pr <= 0 || static_cast<size_t>(pr) > CHN_COUNT) continue;
                    size_t w = static_cast<size_t>(pr) >> 6;
                    uint64_t mask = 1ULL << (static_cast<size_t>(pr) & 63);
                    if (!(CHN_BIT[w] & mask)) continue;
                    std::string_view name = sv_at(CHN_dat, CHN_off, static_cast<size_t>(pr - 1));
                    if (!have_local_chn || name < local_min_chn) {
                        local_min_chn = name;
                        have_local_chn = true;
                    }
                }
                if (!have_local_chn) continue;

                // ---- match! update per-thread mins
                std::string_view title = sv_at(TI_dat, TI_off, t_idx);

                if (!h_title || title < loc_title) { loc_title = title; h_title = true; }
                if (!h_info  || local_min_info < loc_info) { loc_info = local_min_info; h_info = true; }
                if (!h_chn   || local_min_chn  < loc_chn)  { loc_chn  = local_min_chn;  h_chn  = true; }
            }

            if (h_chn)   { tmin_chn[tid].assign(loc_chn.data(), loc_chn.size()); have_chn[tid] = 1; }
            if (h_info)  { tmin_info[tid].assign(loc_info.data(), loc_info.size()); have_info[tid] = 1; }
            if (h_title) { tmin_title[tid].assign(loc_title.data(), loc_title.size()); have_title[tid] = 1; }
        }
    }

    // -------------------- reduce + output --------------------
    {
        GENDB_PHASE("output");

        std::string min_chn, min_info, min_title;
        bool h_chn = false, h_info = false, h_title = false;
        for (int i = 0; i < nthreads; i++) {
            if (have_chn[i] && (!h_chn || tmin_chn[i] < min_chn))       { min_chn = tmin_chn[i]; h_chn = true; }
            if (have_info[i] && (!h_info || tmin_info[i] < min_info))   { min_info = tmin_info[i]; h_info = true; }
            if (have_title[i] && (!h_title || tmin_title[i] < min_title)) { min_title = tmin_title[i]; h_title = true; }
        }

        std::string out_path = rdir + "/Q26b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "FATAL: cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "character_name,rating,complete_hero_movie\n");
        if (h_chn && h_info && h_title) {
            std::fprintf(f, "%s,%s,%s\n", min_chn.c_str(), min_info.c_str(), min_title.c_str());
        }
        std::fclose(f);
    }

    return 0;
}
