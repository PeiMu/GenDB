// Q5a: SELECT MIN(t.title) AS typical_european_movie
// FROM company_type ct, info_type it, movie_companies mc, movie_info mi, title t
// WHERE ct.kind='production companies' AND mc.note LIKE '%(theatrical)%'
//   AND mc.note LIKE '%(France)%'
//   AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German')
//   AND t.production_year > 2005
//   AND t.id=mi.movie_id AND t.id=mc.movie_id
//   AND ct.id=mc.company_type_id AND it.id=mi.info_type_id;

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static inline bool note_matches(const char* p, int64_t len) {
    // Length prefilter: must contain both "(theatrical)" (12) and "(France)" (8) → need >= 20
    if (len < 20) return false;
    // memmem for both needles
    static const char N1[] = "(theatrical)";
    static const char N2[] = "(France)";
    const void* h1 = memmem(p, (size_t)len, N1, 12);
    if (!h1) return false;
    const void* h2 = memmem(p, (size_t)len, N2, 8);
    return h2 != nullptr;
}

static inline bool info_in_set(const char* p, int64_t len) {
    // IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German')
    switch (len) {
        case 6:
            // Sweden, Norway, Denish, German
            return (std::memcmp(p, "Sweden", 6) == 0)
                || (std::memcmp(p, "Norway", 6) == 0)
                || (std::memcmp(p, "Denish", 6) == 0)
                || (std::memcmp(p, "German", 6) == 0);
        case 7:
            // Germany, Denmark, Swedish
            return (std::memcmp(p, "Germany", 7) == 0)
                || (std::memcmp(p, "Denmark", 7) == 0)
                || (std::memcmp(p, "Swedish", 7) == 0);
        case 9:
            // Norwegian
            return std::memcmp(p, "Norwegian", 9) == 0;
        default:
            return false;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    GENDB_PHASE("total");
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // ---- Data loading: mmap all needed columns ----
    MmapColumn<char>     ct_kind_dat;
    MmapColumn<int64_t>  ct_kind_off;

    MmapColumn<int32_t>  t_prod_year;
    MmapColumn<int64_t>  t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int32_t>  mc_off_idx;     // _idx: int32 [N+2]
    MmapColumn<int32_t>  mc_ct;          // movie_companies.company_type_id
    MmapColumn<int64_t>  mc_note_off;
    MmapColumn<char>     mc_note_dat;

    MmapColumn<int32_t>  mi_off_idx;     // _idx: int32 [N+2]
    MmapColumn<int64_t>  mi_info_off;
    MmapColumn<char>     mi_info_dat;

    int32_t target_ct_id = -1;
    int32_t title_count = 0;

    {
        GENDB_PHASE("data_loading");
        ct_kind_dat.open(gendb_dir + "/company_type/kind.dat");
        ct_kind_off.open(gendb_dir + "/company_type/kind.off");

        t_prod_year.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_ct.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gendb_dir + "/movie_companies/note.off");
        mc_note_dat.open(gendb_dir + "/movie_companies/note.dat");

        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        title_count = (int32_t)t_prod_year.count;

        // Random-access patterns for probed columns
        mc_off_idx.advise_random();
        mi_off_idx.advise_random();
        mc_ct.advise_random();
        mc_note_off.advise_random();
        mc_note_dat.advise_random();
        mi_info_off.advise_random();
        mi_info_dat.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
    }

    // ---- Resolve target_ct_id by scanning company_type.kind ----
    {
        GENDB_PHASE("resolve_ct");
        static const char NEEDLE[] = "production companies";
        const size_t NEED_LEN = sizeof(NEEDLE) - 1;
        size_t n_ct = ct_kind_off.count - 1;
        for (size_t r = 0; r < n_ct; ++r) {
            int64_t lo = ct_kind_off[r];
            int64_t hi = ct_kind_off[r + 1];
            if ((size_t)(hi - lo) == NEED_LEN &&
                std::memcmp(ct_kind_dat.data + lo, NEEDLE, NEED_LEN) == 0) {
                target_ct_id = (int32_t)(r + 1);  // dense id = row+1
                break;
            }
        }
        if (target_ct_id < 0) {
            std::fprintf(stderr, "Q5a: target_ct_id not found\n");
            return 2;
        }
    }

    // ---- Main scan ----
    std::string global_min;
    bool global_has = false;

    {
        GENDB_PHASE("main_scan");
        int nthreads = omp_get_max_threads();
        if (nthreads > 12) nthreads = 12;
        std::vector<std::string> local_min(nthreads);
        std::vector<char>        local_has(nthreads, 0);

        const int32_t* PROD = t_prod_year.data;
        const int64_t* TOFF = t_title_off.data;
        const char*    TDAT = t_title_dat.data;
        const int32_t* MC_OFF = mc_off_idx.data;
        const int32_t* MC_CT  = mc_ct.data;
        const int64_t* MC_NOFF = mc_note_off.data;
        const char*    MC_NDAT = mc_note_dat.data;
        const int32_t* MI_OFF = mi_off_idx.data;
        const int64_t* MI_IOFF = mi_info_off.data;
        const char*    MI_IDAT = mi_info_dat.data;
        const int32_t  TGT_CT = target_ct_id;

        #pragma omp parallel num_threads(nthreads)
        {
            int tid = omp_get_thread_num();
            std::string cur_min;
            bool cur_has = false;

            #pragma omp for schedule(dynamic, 100000) nowait
            for (int32_t r = 0; r < title_count; ++r) {
                int32_t py = PROD[r];
                if (py <= 2005 || py == INT32_MIN) continue;

                int32_t t_id = r + 1;

                // probe mc
                int32_t mc_lo = MC_OFF[t_id];
                int32_t mc_hi = MC_OFF[t_id + 1];
                if (mc_lo == mc_hi) continue;

                bool mc_ok = false;
                for (int32_t mr = mc_lo; mr < mc_hi; ++mr) {
                    if (MC_CT[mr] != TGT_CT) continue;
                    int64_t nlo = MC_NOFF[mr];
                    int64_t nhi = MC_NOFF[mr + 1];
                    int64_t nlen = nhi - nlo;
                    if (note_matches(MC_NDAT + nlo, nlen)) {
                        mc_ok = true;
                        break;
                    }
                }
                if (!mc_ok) continue;

                // probe mi
                int32_t mi_lo = MI_OFF[t_id];
                int32_t mi_hi = MI_OFF[t_id + 1];
                if (mi_lo == mi_hi) continue;

                bool mi_ok = false;
                for (int32_t ir = mi_lo; ir < mi_hi; ++ir) {
                    int64_t ilo = MI_IOFF[ir];
                    int64_t ihi = MI_IOFF[ir + 1];
                    int64_t ilen = ihi - ilo;
                    if (ilen > 9 || ilen < 6) continue;
                    if (info_in_set(MI_IDAT + ilo, ilen)) {
                        mi_ok = true;
                        break;
                    }
                }
                if (!mi_ok) continue;

                // load title and update MIN
                int64_t tlo = TOFF[r];
                int64_t thi = TOFF[r + 1];
                std::string_view title_sv(TDAT + tlo, (size_t)(thi - tlo));
                if (!cur_has || title_sv < std::string_view(cur_min)) {
                    cur_min.assign(title_sv.data(), title_sv.size());
                    cur_has = true;
                }
            }

            local_min[tid] = std::move(cur_min);
            local_has[tid] = cur_has ? 1 : 0;
        }

        for (int t = 0; t < nthreads; ++t) {
            if (!local_has[t]) continue;
            if (!global_has || local_min[t] < global_min) {
                global_min = local_min[t];
                global_has = true;
            }
        }
    }

    // ---- Output CSV ----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q5a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(fp, "typical_european_movie\n");
        if (global_has) {
            std::fwrite(global_min.data(), 1, global_min.size(), fp);
            std::fputc('\n', fp);
        } else {
            std::fputs("\n", fp);
        }
        std::fclose(fp);
    }

    return 0;
}
