// Q22b — IMDB JOB
// MIN(cn.name), MIN(mi_idx.info), MIN(t.title) under multi-fact join filters.
// Driver: title; for each surviving title probe mk -> mi_idx -> mi -> mc.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <climits>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

static inline bool sv_lt(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    int c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c < 0;
    return a.size() < b.size();
}

static inline bool memmem_fast(const char* hay, size_t hl, const char* nd, size_t nl) {
    if (nl == 0) return true;
    if (nl > hl) return false;
    return memmem(hay, hl, nd, nl) != nullptr;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: q22b <gendb_dir> <results_dir>\n");
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // ------------------------------------------------------------------
    // Phase 1: open all mmaps
    // ------------------------------------------------------------------
    MmapColumn<int32_t> title_id, title_kind_id, title_prod_year;
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;

    MmapColumn<int32_t> mk_off, mk_keyword_id;
    MmapColumn<int32_t> mi_off, mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int32_t> miidx_off, miidx_info_type_id;
    MmapColumn<int64_t> miidx_info_off;
    MmapColumn<char>    miidx_info_dat;
    MmapColumn<int32_t> mc_off, mc_company_id;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<char>    mc_note_dat;

    MmapColumn<int16_t> cn_country;
    MmapColumn<int64_t> cn_name_off, cn_cc_dict_off;
    MmapColumn<char>    cn_name_dat, cn_cc_dict_dat;

    MmapColumn<int64_t> it_info_off;
    MmapColumn<char>    it_info_dat;

    MmapColumn<int32_t> kw_id;
    MmapColumn<int64_t> kw_kw_off;
    MmapColumn<char>    kw_kw_dat;

    MmapColumn<int32_t> kt_id;
    MmapColumn<int64_t> kt_kind_off;
    MmapColumn<char>    kt_kind_dat;

    {
        GENDB_PHASE("data_loading");
        title_id.open(gendb_dir + "/title/id.bin");
        title_kind_id.open(gendb_dir + "/title/kind_id.bin");
        title_prod_year.open(gendb_dir + "/title/production_year.bin");
        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");

        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        miidx_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_note_off.open(gendb_dir + "/movie_companies/note.off");
        mc_note_dat.open(gendb_dir + "/movie_companies/note.dat");

        cn_country.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");
        cn_cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        kw_id.open(gendb_dir + "/keyword/id.bin");
        kw_kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        kt_id.open(gendb_dir + "/kind_type/id.bin");
        kt_kind_off.open(gendb_dir + "/kind_type/kind.off");
        kt_kind_dat.open(gendb_dir + "/kind_type/kind.dat");
    }

    // ------------------------------------------------------------------
    // Phase 2: resolve dim values
    // ------------------------------------------------------------------
    int32_t it1_id = -1, it2_id = -1; // 'countries', 'rating'
    int16_t us_code = -1;
    std::vector<uint8_t> non_us_cn_mask;       // sized to cn rows; mask[id-1] true if non-US
    std::vector<uint8_t> kw_id_mask;           // mask[id-1] for keyword
    std::vector<uint8_t> kt_id_mask;           // mask[id-1] for kind_type
    // For projection: cn.name lookup by id (id is dense identity starting at 1)

    {
        GENDB_PHASE("dim_resolution");

        // info_type
        size_t it_n = it_info_off.count - 1;
        for (size_t i = 0; i < it_n; i++) {
            int64_t s = it_info_off[i], e = it_info_off[i+1];
            size_t L = e - s;
            const char* p = &it_info_dat[s];
            if (L == 9 && std::memcmp(p, "countries", 9) == 0) it1_id = (int32_t)(i + 1);
            else if (L == 6 && std::memcmp(p, "rating", 6) == 0) it2_id = (int32_t)(i + 1);
        }

        // company_name country_code dict
        size_t dict_n = cn_cc_dict_off.count - 1;
        for (size_t i = 0; i < dict_n; i++) {
            int64_t s = cn_cc_dict_off[i], e = cn_cc_dict_off[i+1];
            size_t L = e - s;
            if (L == 4 && std::memcmp(&cn_cc_dict_dat[s], "[us]", 4) == 0) {
                // Dict codes in column are 1-based (0 reserved for NULL).
                us_code = (int16_t)(i + 1);
                break;
            }
        }
        // Build non-US mask over cn rows
        size_t cn_n = cn_country.count;
        non_us_cn_mask.assign(cn_n, 0);
        for (size_t i = 0; i < cn_n; i++) {
            int16_t c = cn_country[i];
            if (c != 0 && c != us_code) non_us_cn_mask[i] = 1;
        }

        // keyword: 'murder','murder-in-title','blood','violence'
        size_t kw_n = kw_kw_off.count - 1;
        kw_id_mask.assign(kw_n, 0);
        for (size_t i = 0; i < kw_n; i++) {
            int64_t s = kw_kw_off[i], e = kw_kw_off[i+1];
            size_t L = e - s;
            const char* p = &kw_kw_dat[s];
            if ((L == 6 && std::memcmp(p, "murder", 6) == 0) ||
                (L == 15 && std::memcmp(p, "murder-in-title", 15) == 0) ||
                (L == 5 && std::memcmp(p, "blood", 5) == 0) ||
                (L == 8 && std::memcmp(p, "violence", 8) == 0)) {
                kw_id_mask[i] = 1;
            }
        }

        // kind_type: 'movie','episode'
        size_t kt_n = kt_kind_off.count - 1;
        kt_id_mask.assign(kt_n, 0);
        for (size_t i = 0; i < kt_n; i++) {
            int64_t s = kt_kind_off[i], e = kt_kind_off[i+1];
            size_t L = e - s;
            const char* p = &kt_kind_dat[s];
            if ((L == 5 && std::memcmp(p, "movie", 5) == 0) ||
                (L == 7 && std::memcmp(p, "episode", 7) == 0)) {
                kt_id_mask[i] = 1;
            }
        }

        if (it1_id < 0 || it2_id < 0 || us_code < 0) {
            std::fprintf(stderr, "dim resolution failed: it1=%d it2=%d us=%d\n", it1_id, it2_id, (int)us_code);
            return 2;
        }
    }

    // ------------------------------------------------------------------
    // Phase 3: main scan with morsel-driven parallelism
    // ------------------------------------------------------------------
    const size_t N = title_id.count;
    const int32_t* tkid = title_kind_id.data;
    const int32_t* tpy  = title_prod_year.data;
    const int32_t* tid  = title_id.data;
    const int64_t* toff = title_off.data;
    const char*    tdat = title_dat.data;

    const int32_t* mkoff = mk_off.data;
    const int32_t* mkkw  = mk_keyword_id.data;

    const int32_t* mioff = mi_off.data;
    const int32_t* miit  = mi_info_type_id.data;
    const int64_t* mi_inf_off = mi_info_off.data;
    const char*    mi_inf_dat = mi_info_dat.data;

    const int32_t* mxoff = miidx_off.data;
    const int32_t* mxit  = miidx_info_type_id.data;
    const int64_t* mx_inf_off = miidx_info_off.data;
    const char*    mx_inf_dat = miidx_info_dat.data;

    const int32_t* mcoff = mc_off.data;
    const int32_t* mccid = mc_company_id.data;
    const int64_t* mcn_off = mc_note_off.data;
    const char*    mcn_dat = mc_note_dat.data;

    const uint8_t* kw_mask_p = kw_id_mask.data();
    const size_t   kw_mask_n = kw_id_mask.size();
    const uint8_t* kt_mask_p = kt_id_mask.data();
    const size_t   kt_mask_n = kt_id_mask.size();
    const uint8_t* cn_mask_p = non_us_cn_mask.data();
    const size_t   cn_mask_n = non_us_cn_mask.size();

    const int64_t* cn_n_off = cn_name_off.data;
    const char*    cn_n_dat = cn_name_dat.data;

    // mi info set: Germany, German, USA, American
    auto mi_info_match = [](const char* p, size_t L) -> bool {
        switch (L) {
            case 3: return std::memcmp(p, "USA", 3) == 0;
            case 6: return std::memcmp(p, "German", 6) == 0;
            case 7: return std::memcmp(p, "Germany", 7) == 0;
            case 8: return std::memcmp(p, "American", 8) == 0;
            default: return false;
        }
    };

    const char* kThreshold = "7.0";
    const size_t kThresholdLen = 3;

    int nthreads = std::max(1u, std::thread::hardware_concurrency());
    if (nthreads > 12) nthreads = 12;

    struct TLState {
        std::string min_cn_name;
        std::string min_miidx_info;
        std::string min_title;
        bool have = false;
    };
    std::vector<TLState> states(nthreads);

    const size_t MORSEL = 32768;
    std::atomic<size_t> next_morsel{0};

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> workers;
        for (int t = 0; t < nthreads; t++) {
            workers.emplace_back([&, t]() {
                TLState& st = states[t];
                while (true) {
                    size_t start = next_morsel.fetch_add(MORSEL, std::memory_order_relaxed);
                    if (start >= N) break;
                    size_t end = std::min(start + MORSEL, N);
                    for (size_t i = start; i < end; i++) {
                        // Filter: production_year > 2009 (skip INT32_MIN)
                        int32_t py = tpy[i];
                        if (py <= 2009) continue;
                        if (py == INT32_MIN) continue;
                        // Filter: kind_id in kt_ids
                        int32_t kid = tkid[i];
                        if (kid <= 0 || (size_t)kid > kt_mask_n || !kt_mask_p[kid - 1]) continue;

                        int32_t tv = tid[i]; // title id (== i+1 typically; use directly)
                        // movie_keyword probe
                        int32_t mk_lo = mkoff[tv], mk_hi = mkoff[tv + 1];
                        bool has_mk = false;
                        for (int32_t r = mk_lo; r < mk_hi; r++) {
                            int32_t kw = mkkw[r];
                            if (kw > 0 && (size_t)kw <= kw_mask_n && kw_mask_p[kw - 1]) {
                                has_mk = true; break;
                            }
                        }
                        if (!has_mk) continue;

                        // movie_info_idx probe: info_type_id == it2 AND info < '7.0'
                        int32_t mx_lo = mxoff[tv], mx_hi = mxoff[tv + 1];
                        std::string_view best_miidx;
                        bool have_mx = false;
                        for (int32_t r = mx_lo; r < mx_hi; r++) {
                            if (mxit[r] != it2_id) continue;
                            int64_t s = mx_inf_off[r], e = mx_inf_off[r + 1];
                            size_t L = (size_t)(e - s);
                            const char* p = &mx_inf_dat[s];
                            // info < '7.0' (lex)
                            size_t cmpL = std::min(L, kThresholdLen);
                            int c = std::memcmp(p, kThreshold, cmpL);
                            bool less = (c < 0) || (c == 0 && L < kThresholdLen);
                            if (!less) continue;
                            std::string_view sv(p, L);
                            if (!have_mx || sv_lt(sv, best_miidx)) {
                                best_miidx = sv;
                                have_mx = true;
                            }
                        }
                        if (!have_mx) continue;

                        // movie_info probe: info_type_id == it1 AND info IN set
                        int32_t mi_lo = mioff[tv], mi_hi = mioff[tv + 1];
                        bool has_mi = false;
                        for (int32_t r = mi_lo; r < mi_hi; r++) {
                            if (miit[r] != it1_id) continue;
                            int64_t s = mi_inf_off[r], e = mi_inf_off[r + 1];
                            size_t L = (size_t)(e - s);
                            if (mi_info_match(&mi_inf_dat[s], L)) { has_mi = true; break; }
                        }
                        if (!has_mi) continue;

                        // movie_companies probe + cn.name capture
                        int32_t mc_lo = mcoff[tv], mc_hi = mcoff[tv + 1];
                        std::string_view best_cn;
                        bool have_cn = false;
                        for (int32_t r = mc_lo; r < mc_hi; r++) {
                            // note non-empty, contains '(200', NOT contains '(USA)'
                            int64_t s = mcn_off[r], e = mcn_off[r + 1];
                            size_t L = (size_t)(e - s);
                            if (L == 0) continue;
                            const char* note = &mcn_dat[s];
                            if (!memmem_fast(note, L, "(200", 4)) continue;
                            if (memmem_fast(note, L, "(USA)", 5)) continue;
                            // company_id check via non-us mask
                            int32_t cid = mccid[r];
                            if (cid <= 0 || (size_t)cid > cn_mask_n || !cn_mask_p[cid - 1]) continue;
                            // capture cn.name
                            int64_t ns = cn_n_off[cid - 1], ne = cn_n_off[cid];
                            std::string_view sv(&cn_n_dat[ns], (size_t)(ne - ns));
                            if (!have_cn || sv_lt(sv, best_cn)) {
                                best_cn = sv;
                                have_cn = true;
                            }
                        }
                        if (!have_cn) continue;

                        // Surviving title: t.title
                        int64_t ts = toff[i], te = toff[i + 1];
                        std::string_view t_sv(&tdat[ts], (size_t)(te - ts));

                        // Update thread-local mins
                        if (!st.have) {
                            st.min_cn_name.assign(best_cn);
                            st.min_miidx_info.assign(best_miidx);
                            st.min_title.assign(t_sv);
                            st.have = true;
                        } else {
                            if (sv_lt(best_cn, st.min_cn_name)) st.min_cn_name.assign(best_cn);
                            if (sv_lt(best_miidx, st.min_miidx_info)) st.min_miidx_info.assign(best_miidx);
                            if (sv_lt(t_sv, st.min_title)) st.min_title.assign(t_sv);
                        }
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Reduce
    std::string min_cn, min_mx, min_t;
    bool any = false;
    for (auto& st : states) {
        if (!st.have) continue;
        if (!any) {
            min_cn = st.min_cn_name;
            min_mx = st.min_miidx_info;
            min_t  = st.min_title;
            any = true;
        } else {
            if (sv_lt(st.min_cn_name,   min_cn)) min_cn = st.min_cn_name;
            if (sv_lt(st.min_miidx_info, min_mx)) min_mx = st.min_miidx_info;
            if (sv_lt(st.min_title,      min_t))  min_t  = st.min_title;
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: output CSV
    // ------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q22b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "movie_company,rating,western_violent_movie\n");
        if (any) {
            std::fprintf(f, "%.*s,%.*s,%.*s\n",
                         (int)min_cn.size(), min_cn.data(),
                         (int)min_mx.size(), min_mx.data(),
                         (int)min_t.size(),  min_t.data());
        }
        std::fclose(f);
    }

    return 0;
}
