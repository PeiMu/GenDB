// Q28a — GenDB generated implementation
// Strategy:
//   driver = title(year>2000, kind_id ∈ {movie,episode});
//   probe order: mk → cc → mi_idx → mc → mi
//   per-thread MIN(cn.name), MIN(mi_idx.info), MIN(t.title); reduced at end.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <sys/stat.h>
#include <omp.h>

#include "mmap_utils.h"
#include "timing_utils.h"
#include "cli_params.h"

using namespace gendb;

// ------------------------------ helpers ---------------------------------

static int find_varlen_id(const int64_t* off, const char* dat, size_t n,
                          const char* needle, size_t nlen) {
    for (size_t i = 0; i < n; i++) {
        size_t len = (size_t)(off[i+1] - off[i]);
        if (len == nlen && std::memcmp(dat + off[i], needle, nlen) == 0)
            return (int)(i + 1);
    }
    return -1;
}

// dict code: code i+1 references entry i in (.dict.off,.dict.dat); code 0 == NULL
static int find_dict_code(const int64_t* off, const char* dat, size_t k,
                          const char* needle, size_t nlen) {
    for (size_t i = 0; i < k; i++) {
        size_t len = (size_t)(off[i+1] - off[i]);
        if (len == nlen && std::memcmp(dat + off[i], needle, nlen) == 0)
            return (int)(i + 1);
    }
    return -1;
}

static inline int lex_cmp(const uint8_t* a, size_t alen, const uint8_t* b, size_t blen) {
    size_t m = alen < blen ? alen : blen;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

// memmem-ish: returns pointer to first occurrence of needle in hay or nullptr.
static inline const uint8_t* find_sub(const uint8_t* hay, size_t hlen,
                                      const char* needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return nullptr;
    const uint8_t* end = hay + (hlen - nlen);
    uint8_t first = (uint8_t)needle[0];
    for (const uint8_t* p = hay; p <= end; ++p) {
        if (*p == first && std::memcmp(p, needle, nlen) == 0) return p;
    }
    return nullptr;
}

// mi.info ∈ {Sweden, Norway, Germany, Denmark, Swedish, Danish, Norwegian, German, USA, American}
static inline bool mi_info_match(const uint8_t* s, size_t l) {
    switch (l) {
    case 3: return std::memcmp(s, "USA", 3) == 0;
    case 6:
        return std::memcmp(s, "Sweden", 6) == 0 ||
               std::memcmp(s, "Norway", 6) == 0 ||
               std::memcmp(s, "Danish", 6) == 0 ||
               std::memcmp(s, "German", 6) == 0;
    case 7:
        return std::memcmp(s, "Germany", 7) == 0 ||
               std::memcmp(s, "Denmark", 7) == 0 ||
               std::memcmp(s, "Swedish", 7) == 0;
    case 8: return std::memcmp(s, "American", 8) == 0;
    case 9: return std::memcmp(s, "Norwegian", 9) == 0;
    default: return false;
    }
}

// Update a thread-local best (smaller) string in-place.
static inline void update_min(std::string& best, bool& has, const uint8_t* p, size_t len) {
    if (!has) {
        best.assign((const char*)p, len);
        has = true;
        return;
    }
    const uint8_t* bp = (const uint8_t*)best.data();
    if (lex_cmp(p, len, bp, best.size()) < 0) {
        best.assign((const char*)p, len);
    }
}

// ------------------------------ main ------------------------------------

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string sdir = argv[1];
    std::string rdir = argv[2];
    ::mkdir(rdir.c_str(), 0755);

    // -------- data loading (mmap) --------
    MmapColumn<int32_t> t_kind_id, t_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<uint8_t> t_title_dat;

    MmapColumn<int64_t> cct_off; MmapColumn<uint8_t> cct_dat;
    MmapColumn<int64_t> it_off;  MmapColumn<uint8_t> it_dat;
    MmapColumn<int64_t> kt_off;  MmapColumn<uint8_t> kt_dat;
    MmapColumn<int64_t> kw_off;  MmapColumn<uint8_t> kw_dat;

    MmapColumn<uint16_t> cn_cc;                   // company_name.country_code (codes)
    MmapColumn<int64_t> cn_dict_off; MmapColumn<uint8_t> cn_dict_dat;
    MmapColumn<int64_t> cn_name_off; MmapColumn<uint8_t> cn_name_dat;

    MmapColumn<int32_t> mk_keyword_id;            // movie_keyword (sorted by movie_id)
    MmapColumn<int32_t> cc_subject_id, cc_status_id;
    MmapColumn<int32_t> mi_idx_info_type_id;
    MmapColumn<int64_t> mi_idx_info_off; MmapColumn<uint8_t> mi_idx_info_dat;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int64_t> mc_note_off; MmapColumn<uint8_t> mc_note_dat;
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off; MmapColumn<uint8_t> mi_info_dat;

    MmapColumn<int32_t> off_mk, off_cc, off_mii, off_mc, off_mi;

    size_t n_titles = 0, n_cct = 0, n_it = 0, n_kt = 0, n_kw = 0;
    size_t n_cn = 0, n_cn_dict = 0;

    {
        GENDB_PHASE("data_loading");

        t_kind_id.open(sdir + "/title/kind_id.bin");
        t_year.open(sdir + "/title/production_year.bin");
        t_title_off.open(sdir + "/title/title.off");
        t_title_dat.open(sdir + "/title/title.dat");
        n_titles = t_kind_id.count;

        cct_off.open(sdir + "/comp_cast_type/kind.off");
        cct_dat.open(sdir + "/comp_cast_type/kind.dat");
        n_cct = cct_off.count - 1;

        it_off.open(sdir + "/info_type/info.off");
        it_dat.open(sdir + "/info_type/info.dat");
        n_it = it_off.count - 1;

        kt_off.open(sdir + "/kind_type/kind.off");
        kt_dat.open(sdir + "/kind_type/kind.dat");
        n_kt = kt_off.count - 1;

        kw_off.open(sdir + "/keyword/keyword.off");
        kw_dat.open(sdir + "/keyword/keyword.dat");
        n_kw = kw_off.count - 1;

        cn_cc.open(sdir + "/company_name/country_code.bin");
        cn_dict_off.open(sdir + "/company_name/country_code.dict.off");
        cn_dict_dat.open(sdir + "/company_name/country_code.dict.dat");
        cn_name_off.open(sdir + "/company_name/name.off");
        cn_name_dat.open(sdir + "/company_name/name.dat");
        n_cn = cn_cc.count;
        n_cn_dict = cn_dict_off.count - 1;

        mk_keyword_id.open(sdir + "/movie_keyword/keyword_id.bin");
        cc_subject_id.open(sdir + "/complete_cast/subject_id.bin");
        cc_status_id.open(sdir + "/complete_cast/status_id.bin");
        mi_idx_info_type_id.open(sdir + "/movie_info_idx/info_type_id.bin");
        mi_idx_info_off.open(sdir + "/movie_info_idx/info.off");
        mi_idx_info_dat.open(sdir + "/movie_info_idx/info.dat");
        mc_company_id.open(sdir + "/movie_companies/company_id.bin");
        mc_note_off.open(sdir + "/movie_companies/note.off");
        mc_note_dat.open(sdir + "/movie_companies/note.dat");
        mi_info_type_id.open(sdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(sdir + "/movie_info/info.off");
        mi_info_dat.open(sdir + "/movie_info/info.dat");

        off_mk.open(sdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        off_cc.open(sdir + "/_idx/complete_cast__movie_id__offsets.bin");
        off_mii.open(sdir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        off_mc.open(sdir + "/_idx/movie_companies__movie_id__offsets.bin");
        off_mi.open(sdir + "/_idx/movie_info__movie_id__offsets.bin");
    }

    // -------- dim resolution + dim_set builds --------
    int crew_id = -1, verified_id = -1;
    int it1_id = -1, it2_id = -1;
    int kt_movie_id = -1, kt_episode_id = -1;
    int us_code = -1;
    std::vector<int> kw_ids;
    kw_ids.reserve(4);
    std::vector<uint8_t> cn_bits;   // cn_set: cn.id ∈ [1..n_cn]; size n_cn+1 bits

    int year_threshold = 0;

    {
        GENDB_PHASE("dim_resolve");

        crew_id     = find_varlen_id(cct_off.data, (const char*)cct_dat.data, n_cct, "crew", 4);
        verified_id = find_varlen_id(cct_off.data, (const char*)cct_dat.data, n_cct, "complete+verified", 17);
        it1_id      = find_varlen_id(it_off.data,  (const char*)it_dat.data,  n_it,  "countries", 9);
        it2_id      = find_varlen_id(it_off.data,  (const char*)it_dat.data,  n_it,  "rating", 6);
        kt_movie_id   = find_varlen_id(kt_off.data, (const char*)kt_dat.data, n_kt, "movie", 5);
        kt_episode_id = find_varlen_id(kt_off.data, (const char*)kt_dat.data, n_kt, "episode", 7);
        us_code = find_dict_code(cn_dict_off.data, (const char*)cn_dict_dat.data, n_cn_dict, "[us]", 4);

        // kw_set: 4 keyword ids
        const char* needles[4] = {"murder", "murder-in-title", "blood", "violence"};
        size_t nlen[4] = {6, 15, 5, 8};
        for (size_t i = 0; i < n_kw; i++) {
            size_t l = (size_t)(kw_off[i+1] - kw_off[i]);
            const uint8_t* p = kw_dat.data + kw_off[i];
            for (int k = 0; k < 4; k++) {
                if (l == nlen[k] && std::memcmp(p, needles[k], l) == 0) {
                    kw_ids.push_back((int)(i + 1));
                    break;
                }
            }
            if (kw_ids.size() == 4) break;
        }

        // cn_set bitset: cn.id where country_code != us_code AND country_code != 0
        cn_bits.assign(n_cn + 1, 0);
        uint16_t usc = (uint16_t)us_code;
        for (size_t i = 0; i < n_cn; i++) {
            uint16_t c = cn_cc[i];
            if (c != 0 && c != usc) cn_bits[i + 1] = 1; // cn.id = i+1
        }

        year_threshold = (int)parse_int_arg(argc, argv, "--year", 2000);

        std::fprintf(stderr,
            "[dim] crew=%d verified=%d it1=%d it2=%d kt_movie=%d kt_episode=%d us=%d kw_ids=%zu\n",
            crew_id, verified_id, it1_id, it2_id, kt_movie_id, kt_episode_id, us_code, kw_ids.size());
    }

    // Pre-pack kw_ids into 4-int array (some may not exist, pad with -1 sentinels)
    int kw0 = kw_ids.size() > 0 ? kw_ids[0] : -1;
    int kw1 = kw_ids.size() > 1 ? kw_ids[1] : -1;
    int kw2 = kw_ids.size() > 2 ? kw_ids[2] : -1;
    int kw3 = kw_ids.size() > 3 ? kw_ids[3] : -1;

    const char* eight_five = "8.5";
    const size_t eight_five_len = 3;

    // -------- main scan (morsel-parallel over title.id range) --------
    int num_threads = omp_get_max_threads();
    std::vector<std::string> tls_cn(num_threads), tls_miidx(num_threads), tls_title(num_threads);
    std::vector<uint8_t>     tls_has_cn(num_threads, 0), tls_has_miidx(num_threads, 0), tls_has_title(num_threads, 0);

    {
        GENDB_PHASE("main_scan");

        const int32_t* off_mk_p  = off_mk.data;
        const int32_t* off_cc_p  = off_cc.data;
        const int32_t* off_mii_p = off_mii.data;
        const int32_t* off_mc_p  = off_mc.data;
        const int32_t* off_mi_p  = off_mi.data;

        const int32_t* kind_id_p = t_kind_id.data;
        const int32_t* year_p    = t_year.data;
        const int64_t* title_off_p = t_title_off.data;
        const uint8_t* title_dat_p = t_title_dat.data;

        const int32_t* mk_kid    = mk_keyword_id.data;
        const int32_t* cc_subj   = cc_subject_id.data;
        const int32_t* cc_stat   = cc_status_id.data;
        const int32_t* mii_itid  = mi_idx_info_type_id.data;
        const int64_t* mii_off   = mi_idx_info_off.data;
        const uint8_t* mii_dat   = mi_idx_info_dat.data;
        const int32_t* mc_cid    = mc_company_id.data;
        const int64_t* mc_noff   = mc_note_off.data;
        const uint8_t* mc_ndat   = mc_note_dat.data;
        const int32_t* mi_itid   = mi_info_type_id.data;
        const int64_t* mi_off_p  = mi_info_off.data;
        const uint8_t* mi_dat    = mi_info_dat.data;
        const int64_t* cn_noff   = cn_name_off.data;
        const uint8_t* cn_ndat   = cn_name_dat.data;
        const uint8_t* cn_bits_p = cn_bits.data();

        #pragma omp parallel for schedule(dynamic, 16384)
        for (int32_t t_id = 1; t_id <= (int32_t)n_titles; ++t_id) {
            int row = t_id - 1;
            int y = year_p[row];
            if (y <= year_threshold) continue;
            int kid = kind_id_p[row];
            if (kid != kt_movie_id && kid != kt_episode_id) continue;

            // -- mk semi-join (most selective): keyword_id ∈ kw_set --
            {
                int32_t lo = off_mk_p[t_id], hi = off_mk_p[t_id + 1];
                bool found = false;
                for (int32_t r = lo; r < hi; ++r) {
                    int kw = mk_kid[r];
                    if (kw == kw0 || kw == kw1 || kw == kw2 || kw == kw3) { found = true; break; }
                }
                if (!found) continue;
            }

            // -- cc semi-join: subject==crew AND status!=verified --
            {
                int32_t lo = off_cc_p[t_id], hi = off_cc_p[t_id + 1];
                bool found = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (cc_subj[r] == crew_id && cc_stat[r] != verified_id) { found = true; break; }
                }
                if (!found) continue;
            }

            // -- mi_idx capture (min info for rating < "8.5") --
            const uint8_t* best_miidx_p = nullptr;
            size_t best_miidx_l = 0;
            {
                int32_t lo = off_mii_p[t_id], hi = off_mii_p[t_id + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    if (mii_itid[r] != it2_id) continue;
                    const uint8_t* p = mii_dat + mii_off[r];
                    size_t l = (size_t)(mii_off[r + 1] - mii_off[r]);
                    if (lex_cmp(p, l, (const uint8_t*)eight_five, eight_five_len) >= 0) continue;
                    if (best_miidx_p == nullptr || lex_cmp(p, l, best_miidx_p, best_miidx_l) < 0) {
                        best_miidx_p = p; best_miidx_l = l;
                    }
                }
                if (best_miidx_p == nullptr) continue;
            }

            // -- mc capture (min cn.name across matching mcs) --
            const uint8_t* best_cn_p = nullptr;
            size_t best_cn_l = 0;
            {
                int32_t lo = off_mc_p[t_id], hi = off_mc_p[t_id + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    size_t nlen = (size_t)(mc_noff[r + 1] - mc_noff[r]);
                    if (nlen == 0) continue;                       // note NULL
                    const uint8_t* note = mc_ndat + mc_noff[r];
                    if (find_sub(note, nlen, "(USA)", 5) != nullptr) continue;  // NOT LIKE %(USA)%
                    const uint8_t* hit200 = find_sub(note, nlen, "(200", 4);
                    if (hit200 == nullptr) continue;               // LIKE %(200...)%
                    // require a ')' byte at some position after hit200 (anywhere after the '(200' match)
                    size_t after = (size_t)((hit200 - note) + 4);
                    bool has_close = false;
                    for (size_t k = after; k < nlen; ++k) { if (note[k] == ')') { has_close = true; break; } }
                    if (!has_close) continue;

                    int cid = mc_cid[r];
                    if (cid <= 0 || (size_t)cid > n_cn) continue;
                    if (!cn_bits_p[cid]) continue;

                    const uint8_t* nm = cn_ndat + cn_noff[cid - 1];
                    size_t nm_l = (size_t)(cn_noff[cid] - cn_noff[cid - 1]);
                    if (best_cn_p == nullptr || lex_cmp(nm, nm_l, best_cn_p, best_cn_l) < 0) {
                        best_cn_p = nm; best_cn_l = nm_l;
                    }
                }
                if (best_cn_p == nullptr) continue;
            }

            // -- mi semi-join (LAST): info_type==it1 AND info ∈ mi_info_set --
            {
                int32_t lo = off_mi_p[t_id], hi = off_mi_p[t_id + 1];
                bool found = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mi_itid[r] != it1_id) continue;
                    const uint8_t* p = mi_dat + mi_off_p[r];
                    size_t l = (size_t)(mi_off_p[r + 1] - mi_off_p[r]);
                    if (mi_info_match(p, l)) { found = true; break; }
                }
                if (!found) continue;
            }

            // -- title passes all predicates: update thread-local MINs --
            int tid_thr = omp_get_thread_num();
            update_min(tls_miidx[tid_thr], (bool&)tls_has_miidx[tid_thr], best_miidx_p, best_miidx_l);
            update_min(tls_cn[tid_thr],    (bool&)tls_has_cn[tid_thr],    best_cn_p,    best_cn_l);

            const uint8_t* tt_p = title_dat_p + title_off_p[row];
            size_t tt_l = (size_t)(title_off_p[row + 1] - title_off_p[row]);
            update_min(tls_title[tid_thr], (bool&)tls_has_title[tid_thr], tt_p, tt_l);
        }
    }

    // -------- reduction --------
    std::string min_cn, min_miidx, min_title;
    bool has_cn = false, has_miidx = false, has_title = false;
    for (int t = 0; t < num_threads; t++) {
        if (tls_has_cn[t]) update_min(min_cn, has_cn,
            (const uint8_t*)tls_cn[t].data(), tls_cn[t].size());
        if (tls_has_miidx[t]) update_min(min_miidx, has_miidx,
            (const uint8_t*)tls_miidx[t].data(), tls_miidx[t].size());
        if (tls_has_title[t]) update_min(min_title, has_title,
            (const uint8_t*)tls_title[t].data(), tls_title[t].size());
    }

    // -------- output --------
    {
        GENDB_PHASE("output");
        std::string outp = rdir + "/Q28a.csv";
        FILE* f = std::fopen(outp.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", outp.c_str()); return 2; }
        std::fprintf(f, "movie_company,rating,complete_euro_dark_movie\n");
        if (has_cn && has_miidx && has_title) {
            std::fprintf(f, "%.*s,%.*s,%.*s\n",
                (int)min_cn.size(), min_cn.data(),
                (int)min_miidx.size(), min_miidx.data(),
                (int)min_title.size(), min_title.data());
        } else {
            std::fprintf(f, ",,\n");
        }
        std::fclose(f);
    }

    return 0;
}
