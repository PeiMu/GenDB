// Q26c — MIN(chn.name), MIN(mi_idx.info), MIN(t.title)
// Driver: title (year>2000, kind=movie). Probes: mk, cc, mi_idx, ci.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

// ---------------------------------------------------------------------------
struct StrRef {
    const char* p;
    uint32_t len;
    bool valid;
};

static inline void update_min(StrRef& cur, const char* p, uint32_t len) {
    if (len == 0) return;
    if (!cur.valid) { cur.p = p; cur.len = len; cur.valid = true; return; }
    uint32_t m = cur.len < len ? cur.len : len;
    int c = std::memcmp(p, cur.p, m);
    if (c < 0 || (c == 0 && len < cur.len)) {
        cur.p = p; cur.len = len; cur.valid = true;
    }
}

static inline void merge_min(StrRef& cur, const StrRef& other) {
    if (!other.valid) return;
    update_min(cur, other.p, other.len);
}

// ---------------------------------------------------------------------------
static inline bool varlen_eq(const int64_t* off, const char* dat,
                             int32_t i, const char* lit, size_t lit_len) {
    int64_t s = off[i], e = off[i+1];
    if ((size_t)(e - s) != lit_len) return false;
    return std::memcmp(dat + s, lit, lit_len) == 0;
}

static inline const void* memmem_safe(const void* hay, size_t hlen,
                                      const void* needle, size_t nlen) {
#if defined(_GNU_SOURCE) || defined(__GLIBC__)
    return memmem(hay, hlen, needle, nlen);
#else
    if (nlen == 0) return hay;
    if (hlen < nlen) return nullptr;
    const char* h = (const char*)hay;
    const char* n = (const char*)needle;
    for (size_t i = 0; i <= hlen - nlen; ++i) {
        if (h[i] == n[0] && std::memcmp(h+i, n, nlen) == 0) return h+i;
    }
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];

    // Ensure results dir exists
    {
        std::string cmd = "mkdir -p '" + rdir + "'";
        if (std::system(cmd.c_str()) != 0) { /* ignore */ }
    }

    // -----------------------------------------------------------------------
    // Phase 1: mmap all columns + indexes
    // -----------------------------------------------------------------------
    MmapColumn<int64_t> kt_kind_off, it_info_off, cct_kind_off;
    MmapColumn<char>    kt_kind_dat, it_info_dat, cct_kind_dat;
    MmapColumn<int32_t> kt_id, it_id, cct_id;

    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;
    MmapColumn<int32_t> kw_id;

    MmapColumn<int64_t> chn_off;
    MmapColumn<char>    chn_dat;
    MmapColumn<int32_t> chn_id;

    MmapColumn<int32_t> t_id, t_kind_id, t_prod_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t> mk_off_idx;

    MmapColumn<int32_t> cc_movie_id, cc_subject_id, cc_status_id;
    MmapColumn<int32_t> cc_off_idx;

    MmapColumn<int32_t> miidx_movie_id, miidx_info_type_id;
    MmapColumn<int64_t> miidx_info_off;
    MmapColumn<char>    miidx_info_dat;
    MmapColumn<int32_t> miidx_off_idx;

    MmapColumn<int32_t> ci_movie_id, ci_person_role_id;
    MmapColumn<int32_t> ci_off_idx;

    {
        GENDB_PHASE("data_loading");

        kt_kind_off.open(gdir + "/kind_type/kind.off");
        kt_kind_dat.open(gdir + "/kind_type/kind.dat");
        kt_id.open(gdir + "/kind_type/id.bin");

        it_info_off.open(gdir + "/info_type/info.off");
        it_info_dat.open(gdir + "/info_type/info.dat");
        it_id.open(gdir + "/info_type/id.bin");

        cct_kind_off.open(gdir + "/comp_cast_type/kind.off");
        cct_kind_dat.open(gdir + "/comp_cast_type/kind.dat");
        cct_id.open(gdir + "/comp_cast_type/id.bin");

        kw_off.open(gdir + "/keyword/keyword.off");
        kw_dat.open(gdir + "/keyword/keyword.dat");
        kw_id.open(gdir + "/keyword/id.bin");

        chn_off.open(gdir + "/char_name/name.off");
        chn_dat.open(gdir + "/char_name/name.dat");
        chn_id.open(gdir + "/char_name/id.bin");

        t_id.open(gdir + "/title/id.bin");
        t_kind_id.open(gdir + "/title/kind_id.bin");
        t_prod_year.open(gdir + "/title/production_year.bin");
        t_title_off.open(gdir + "/title/title.off");
        t_title_dat.open(gdir + "/title/title.dat");

        mk_movie_id.open(gdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");
        mk_off_idx.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");

        cc_movie_id.open(gdir + "/complete_cast/movie_id.bin");
        cc_subject_id.open(gdir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gdir + "/complete_cast/status_id.bin");
        cc_off_idx.open(gdir + "/_idx/complete_cast__movie_id__offsets.bin");

        miidx_movie_id.open(gdir + "/movie_info_idx/movie_id.bin");
        miidx_info_type_id.open(gdir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gdir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gdir + "/movie_info_idx/info.dat");
        miidx_off_idx.open(gdir + "/_idx/movie_info_idx__movie_id__offsets.bin");

        ci_movie_id.open(gdir + "/cast_info/movie_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_off_idx.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");

        // Random-access readahead for FK/index columns
        mk_keyword_id.advise_random();
        ci_person_role_id.advise_random();
        cc_subject_id.advise_random();
        cc_status_id.advise_random();
        miidx_info_type_id.advise_random();
        chn_off.advise_random();
        chn_dat.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
        miidx_info_off.advise_random();
        miidx_info_dat.advise_random();
    }

    // -----------------------------------------------------------------------
    // Phase 2: Resolve scalar dim IDs + build bitsets
    // -----------------------------------------------------------------------
    int32_t movie_kind_id = -1;
    int32_t it2_id = -1;
    int32_t cct1_id = -1;
    uint8_t complete_status_mask = 0; // bits indexed by cct.id (small)
    int32_t cct_max_id = 0;

    std::vector<uint64_t> kw_set;
    std::vector<uint64_t> chn_set;

    int32_t kw_max_id = 0;
    int32_t chn_max_id = 0;

    {
        GENDB_PHASE("build_dim_scalars");

        // kind_type: find 'movie'
        for (size_t i = 0; i < kt_id.size(); ++i) {
            if (varlen_eq(kt_kind_off.data, kt_kind_dat.data, (int32_t)i, "movie", 5)) {
                movie_kind_id = kt_id[i];
                break;
            }
        }

        // info_type: find 'rating'
        for (size_t i = 0; i < it_id.size(); ++i) {
            if (varlen_eq(it_info_off.data, it_info_dat.data, (int32_t)i, "rating", 6)) {
                it2_id = it_id[i];
                break;
            }
        }

        // comp_cast_type: cct1 = 'cast'; complete_status_set = LIKE '%complete%'
        for (size_t i = 0; i < cct_id.size(); ++i) {
            int64_t s = cct_kind_off[i], e = cct_kind_off[i+1];
            uint32_t len = (uint32_t)(e - s);
            const char* p = cct_kind_dat.data + s;
            int32_t id = cct_id[i];
            if (id > cct_max_id) cct_max_id = id;
            if (len == 4 && std::memcmp(p, "cast", 4) == 0) {
                cct1_id = id;
            }
            if (memmem_safe(p, len, "complete", 8) != nullptr) {
                if (id >= 0 && id < 64) complete_status_mask |= (uint8_t)(1u << id);
            }
        }

        if (movie_kind_id < 0 || it2_id < 0 || cct1_id < 0 || complete_status_mask == 0) {
            // No matches possible
            std::string out_path = rdir + "/Q26c.csv";
            FILE* f = std::fopen(out_path.c_str(), "w");
            if (f) {
                std::fprintf(f, "character_name,rating,complete_hero_movie\n");
                std::fclose(f);
            }
            return 0;
        }
    }

    {
        GENDB_PHASE("build_kw_set");
        // 10 keywords
        static const char* KWS[10] = {
            "superhero","marvel-comics","based-on-comic","tv-special",
            "fight","violence","magnet","web","claw","laser"
        };
        size_t KL[10];
        for (int i = 0; i < 10; ++i) KL[i] = std::strlen(KWS[i]);

        // find max id
        for (size_t i = 0; i < kw_id.size(); ++i) {
            int32_t id = kw_id[i];
            if (id > kw_max_id) kw_max_id = id;
        }
        kw_set.assign((kw_max_id / 64) + 1, 0);

        for (size_t i = 0; i < kw_id.size(); ++i) {
            int64_t s = kw_off[i], e = kw_off[i+1];
            uint32_t len = (uint32_t)(e - s);
            const char* p = kw_dat.data + s;
            for (int j = 0; j < 10; ++j) {
                if (len == KL[j] && std::memcmp(p, KWS[j], len) == 0) {
                    int32_t id = kw_id[i];
                    kw_set[id >> 6] |= (1ULL << (id & 63));
                    break;
                }
            }
        }
    }

    {
        GENDB_PHASE("build_chn_set");
        // dense PK so id == row+1 typically; use max id
        // chn_id is dense per guide; find max via last element (sorted by id)
        chn_max_id = chn_id.size() > 0 ? chn_id[chn_id.size() - 1] : 0;
        chn_set.assign((chn_max_id / 64) + 1, 0);

        size_t N = chn_id.size();
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < N; ++i) {
            int64_t s = chn_off[i], e = chn_off[i+1];
            uint32_t len = (uint32_t)(e - s);
            if (len == 0) continue;
            const char* p = chn_dat.data + s;
            const void* m1 = memmem_safe(p, len, "man", 3);
            const void* m2 = (m1 != nullptr) ? m1 : memmem_safe(p, len, "Man", 3);
            if (m2 != nullptr) {
                int32_t id = chn_id[i];
                // atomic-safe via bitset OR — race within a single word possible; use atomic OR
                uint64_t bit = (1ULL << (id & 63));
                __atomic_fetch_or(&chn_set[id >> 6], bit, __ATOMIC_RELAXED);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 3: Driver loop over title — parallel
    // -----------------------------------------------------------------------
    StrRef global_min_chn  = {nullptr, 0, false};
    StrRef global_min_info = {nullptr, 0, false};
    StrRef global_min_title= {nullptr, 0, false};

    {
        GENDB_PHASE("main_scan");

        const int32_t* T_kind  = t_kind_id.data;
        const int32_t* T_year  = t_prod_year.data;
        const int32_t* T_id_ar = t_id.data;
        const int64_t* T_toff  = t_title_off.data;
        const char*    T_tdat  = t_title_dat.data;
        const size_t   N_t     = t_id.size();

        const int32_t* MK_off  = mk_off_idx.data;
        const int32_t* MK_kw   = mk_keyword_id.data;
        const size_t   MK_off_n = mk_off_idx.size();

        const int32_t* CC_off  = cc_off_idx.data;
        const int32_t* CC_sub  = cc_subject_id.data;
        const int32_t* CC_sts  = cc_status_id.data;
        const size_t   CC_off_n = cc_off_idx.size();

        const int32_t* MI_off  = miidx_off_idx.data;
        const int32_t* MI_iti  = miidx_info_type_id.data;
        const int64_t* MI_ioff = miidx_info_off.data;
        const char*    MI_idat = miidx_info_dat.data;
        const size_t   MI_off_n = miidx_off_idx.size();

        const int32_t* CI_off  = ci_off_idx.data;
        const int32_t* CI_prl  = ci_person_role_id.data;
        const size_t   CI_off_n = ci_off_idx.size();

        const uint64_t* KW   = kw_set.data();
        const size_t    KWN  = kw_set.size();
        const uint64_t* CHN  = chn_set.data();
        const size_t    CHNN = chn_set.size();

        const int32_t MOVIE_KIND = movie_kind_id;
        const int32_t IT2        = it2_id;
        const int32_t CCT1       = cct1_id;
        const uint8_t CSTM       = complete_status_mask;

        #pragma omp parallel
        {
            StrRef lmin_chn   = {nullptr, 0, false};
            StrRef lmin_info  = {nullptr, 0, false};
            StrRef lmin_title = {nullptr, 0, false};

            #pragma omp for schedule(dynamic, 8192) nowait
            for (size_t r = 0; r < N_t; ++r) {
                // Title filter
                if (T_kind[r] != MOVIE_KIND) continue;
                int32_t yr = T_year[r];
                if (yr <= 2000) continue; // INT32_MIN (null) also fails

                int32_t tid = T_id_ar[r];

                // --- Step 1: movie_keyword semi-join via kw_set ---
                if ((size_t)tid + 1 >= MK_off_n) continue;
                int32_t mk_lo = MK_off[tid], mk_hi = MK_off[tid + 1];
                bool mk_ok = false;
                for (int32_t k = mk_lo; k < mk_hi; ++k) {
                    int32_t kid = MK_kw[k];
                    if (kid < 0) continue;
                    size_t w = (size_t)(kid >> 6);
                    if (w >= KWN) continue;
                    if (KW[w] & (1ULL << (kid & 63))) { mk_ok = true; break; }
                }
                if (!mk_ok) continue;

                // --- Step 2: complete_cast semi-join (subject==cct1, status in mask) ---
                if ((size_t)tid + 1 >= CC_off_n) continue;
                int32_t cc_lo = CC_off[tid], cc_hi = CC_off[tid + 1];
                bool cc_ok = false;
                for (int32_t k = cc_lo; k < cc_hi; ++k) {
                    if (CC_sub[k] != CCT1) continue;
                    int32_t st = CC_sts[k];
                    if (st < 0 || st >= 64) continue;
                    if (CSTM & (uint8_t)(1u << st)) { cc_ok = true; break; }
                }
                if (!cc_ok) continue;

                // --- Step 3: movie_info_idx (info_type==IT2) — capture info ---
                if ((size_t)tid + 1 >= MI_off_n) continue;
                int32_t mi_lo = MI_off[tid], mi_hi = MI_off[tid + 1];
                StrRef per_t_info = {nullptr, 0, false};
                for (int32_t k = mi_lo; k < mi_hi; ++k) {
                    if (MI_iti[k] != IT2) continue;
                    int64_t s = MI_ioff[k], e = MI_ioff[k+1];
                    uint32_t len = (uint32_t)(e - s);
                    update_min(per_t_info, MI_idat + s, len);
                }
                if (!per_t_info.valid) continue;

                // --- Step 4: cast_info (person_role_id in chn_set) — capture chn.name ---
                if ((size_t)tid + 1 >= CI_off_n) continue;
                int32_t ci_lo = CI_off[tid], ci_hi = CI_off[tid + 1];
                StrRef per_t_chn = {nullptr, 0, false};
                for (int32_t k = ci_lo; k < ci_hi; ++k) {
                    int32_t prl = CI_prl[k];
                    if (prl <= 0) continue; // null = INT32_MIN; also 0 invalid
                    size_t w = (size_t)(prl >> 6);
                    if (w >= CHNN) continue;
                    if (!(CHN[w] & (1ULL << (prl & 63)))) continue;
                    // chn id maps to row index = prl - 1 (dense)
                    int64_t s = chn_off.data[prl - 1], e = chn_off.data[prl];
                    uint32_t len = (uint32_t)(e - s);
                    update_min(per_t_chn, chn_dat.data + s, len);
                }
                if (!per_t_chn.valid) continue;

                // All 4 semi-joins satisfied; commit local mins.
                // Update title min
                {
                    int64_t s = T_toff[r], e = T_toff[r + 1];
                    update_min(lmin_title, T_tdat + s, (uint32_t)(e - s));
                }
                merge_min(lmin_info, per_t_info);
                merge_min(lmin_chn, per_t_chn);
            }

            // Merge into globals
            #pragma omp critical
            {
                merge_min(global_min_chn, lmin_chn);
                merge_min(global_min_info, lmin_info);
                merge_min(global_min_title, lmin_title);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 4: Output CSV
    // -----------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = rdir + "/Q26c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot open output %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "character_name,rating,complete_hero_movie\n");
        auto wr = [&](const StrRef& s) {
            if (s.valid) std::fwrite(s.p, 1, s.len, f);
        };
        wr(global_min_chn);   std::fputc(',', f);
        wr(global_min_info);  std::fputc(',', f);
        wr(global_min_title); std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
