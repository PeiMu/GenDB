// Q22a — IMDB JOB: MIN(cn.name), MIN(mi_idx.info), MIN(t.title) under heavy filters.
// Driver: title.id. For each surviving title, do 4 EXISTS probes via offsets_only indexes.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <unordered_set>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static inline std::string_view sv_from(const char* data, const int64_t* off, int64_t i) {
    int64_t lo = off[i], hi = off[i + 1];
    return std::string_view(data + lo, static_cast<size_t>(hi - lo));
}

// memmem wrapper: search needle in haystack
static inline bool contains_substr(const char* hs, size_t hlen, const char* ndl, size_t nlen) {
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    return memmem(hs, hlen, ndl, nlen) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // --- Data loading: mmap everything we need ---------------------------------
    MmapColumn<int64_t> info_type_info_off;
    MmapColumn<char> info_type_info_dat;

    MmapColumn<int64_t> keyword_kw_off;
    MmapColumn<char> keyword_kw_dat;

    MmapColumn<int64_t> kt_off;
    MmapColumn<char> kt_dat;

    MmapColumn<int64_t> cn_dict_off;
    MmapColumn<char> cn_dict_dat;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int64_t> cn_name_off;
    MmapColumn<char> cn_name_dat;

    MmapColumn<int32_t> t_kind_id;
    MmapColumn<int32_t> t_prod_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char> t_title_dat;

    MmapColumn<int32_t> mk_off_idx;
    MmapColumn<int32_t> mk_keyword_id;

    MmapColumn<int32_t> miidx_off_idx;
    MmapColumn<int32_t> miidx_info_type_id;
    MmapColumn<int64_t> miidx_info_off;
    MmapColumn<char> miidx_info_dat;

    MmapColumn<int32_t> mi_off_idx;
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char> mi_info_dat;

    MmapColumn<int32_t> mc_off_idx;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_company_type_id;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<char> mc_note_dat;

    {
        GENDB_PHASE("data_loading");
        info_type_info_off.open(gendb_dir + "/info_type/info.off");
        info_type_info_dat.open(gendb_dir + "/info_type/info.dat");

        keyword_kw_off.open(gendb_dir + "/keyword/keyword.off");
        keyword_kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        kt_off.open(gendb_dir + "/kind_type/kind.off");
        kt_dat.open(gendb_dir + "/kind_type/kind.dat");

        cn_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");
        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_prod_year.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mk_off_idx.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        miidx_off_idx.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gendb_dir + "/movie_companies/note.off");
        mc_note_dat.open(gendb_dir + "/movie_companies/note.dat");
    }

    // --- Dim resolution ---------------------------------------------------------
    int16_t us_code = 0;
    int32_t it1_id = 0, it2_id = 0;
    std::vector<int32_t> kw_ids_vec;
    int32_t kt_ids[8] = {0,0,0,0,0,0,0,0};
    int kt_count = 0;

    {
        GENDB_PHASE("dim_resolve");

        // country_code dict scan for '[us]'
        // dict entry i (1-based code i+1, since code 0 = NULL); actually code i references dict entry i-1
        size_t dict_n = cn_dict_off.count - 1;
        const char* dat = cn_dict_dat.data;
        for (size_t i = 0; i < dict_n; ++i) {
            int64_t lo = cn_dict_off.data[i], hi = cn_dict_off.data[i+1];
            size_t len = (size_t)(hi - lo);
            if (len == 4 && std::memcmp(dat + lo, "[us]", 4) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }

        // info_type scan for 'countries' and 'rating'
        size_t it_n = info_type_info_off.count - 1;
        const char* it_dat = info_type_info_dat.data;
        for (size_t i = 0; i < it_n; ++i) {
            int64_t lo = info_type_info_off.data[i], hi = info_type_info_off.data[i+1];
            size_t len = (size_t)(hi - lo);
            if (len == 9 && std::memcmp(it_dat + lo, "countries", 9) == 0) {
                it1_id = (int32_t)(i + 1);
            } else if (len == 6 && std::memcmp(it_dat + lo, "rating", 6) == 0) {
                it2_id = (int32_t)(i + 1);
            }
        }

        // keyword scan for {murder, murder-in-title, blood, violence}
        size_t kw_n = keyword_kw_off.count - 1;
        const char* kw_dat = keyword_kw_dat.data;
        const char* targets[4] = {"murder", "murder-in-title", "blood", "violence"};
        size_t target_lens[4] = {6, 15, 5, 8};
        int found = 0;
        for (size_t i = 0; i < kw_n && found < 4; ++i) {
            int64_t lo = keyword_kw_off.data[i], hi = keyword_kw_off.data[i+1];
            size_t len = (size_t)(hi - lo);
            for (int j = 0; j < 4; ++j) {
                if (len == target_lens[j] && std::memcmp(kw_dat + lo, targets[j], len) == 0) {
                    kw_ids_vec.push_back((int32_t)(i + 1));
                    ++found;
                    break;
                }
            }
        }

        // kind_type scan for {movie, episode}
        size_t kt_n = kt_off.count - 1;
        const char* kt_d = kt_dat.data;
        for (size_t i = 0; i < kt_n; ++i) {
            int64_t lo = kt_off.data[i], hi = kt_off.data[i+1];
            size_t len = (size_t)(hi - lo);
            if ((len == 5 && std::memcmp(kt_d + lo, "movie", 5) == 0) ||
                (len == 7 && std::memcmp(kt_d + lo, "episode", 7) == 0)) {
                kt_ids[kt_count++] = (int32_t)(i + 1);
            }
        }

        std::fprintf(stderr, "us_code=%d it1=%d it2=%d kw_n=%zu kt_n=%d\n",
                     (int)us_code, it1_id, it2_id, kw_ids_vec.size(), kt_count);
    }

    // --- Pre-build needle constants -------------------------------------------
    static const char* MI_INFO_LITERALS[4] = {"Germany", "German", "USA", "American"};
    static const size_t MI_INFO_LITERALS_LEN[4] = {7, 6, 3, 8};

    // --- Driver scan with morsel-driven parallelism ----------------------------
    size_t n_titles = t_kind_id.count;  // 2528312
    int n_threads = std::max(1u, std::thread::hardware_concurrency());
    if (n_threads > 12) n_threads = 12;

    struct ThreadMin {
        std::string min_title;   // empty = no result
        std::string min_miidx;
        std::string min_cn;
        bool has = false;
    };
    std::vector<ThreadMin> tmins(n_threads);

    auto worker = [&](int tid, size_t start, size_t end) {
        std::string_view best_t, best_mi, best_cn;
        bool has = false;

        // copy kw_ids into local stack array (cardinality <= 4)
        int32_t kwids_local[4] = {0,0,0,0};
        int kw_cnt = (int)kw_ids_vec.size();
        for (int i = 0; i < kw_cnt; ++i) kwids_local[i] = kw_ids_vec[i];

        const int32_t* kind_id = t_kind_id.data;
        const int32_t* prod_year = t_prod_year.data;
        const int64_t* t_off = t_title_off.data;
        const char* t_dat = t_title_dat.data;

        const int32_t* mk_off = mk_off_idx.data;
        const int32_t* mk_kw = mk_keyword_id.data;

        const int32_t* mii_off = miidx_off_idx.data;
        const int32_t* mii_itid = miidx_info_type_id.data;
        const int64_t* mii_inf_off = miidx_info_off.data;
        const char* mii_inf_dat = miidx_info_dat.data;

        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int64_t* mi_inf_off = mi_info_off.data;
        const char* mi_inf_dat = mi_info_dat.data;

        const int32_t* mc_off = mc_off_idx.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int64_t* mc_n_off = mc_note_off.data;
        const char* mc_n_dat = mc_note_dat.data;

        const int16_t* cn_cc = cn_country_code.data;
        const int64_t* cn_n_off = cn_name_off.data;
        const char* cn_n_dat = cn_name_dat.data;

        for (size_t row = start; row < end; ++row) {
            // (1) production_year > 2008
            int32_t py = prod_year[row];
            if (py == INT32_MIN || py <= 2008) continue;
            // (2) kt_ids.contains(kind_id)
            int32_t ki = kind_id[row];
            bool kt_ok = false;
            for (int i = 0; i < kt_count; ++i) if (kt_ids[i] == ki) { kt_ok = true; break; }
            if (!kt_ok) continue;

            int32_t v = (int32_t)row + 1;  // title.id

            // (3) mk EXISTS
            int32_t lo = mk_off[v], hi = mk_off[v + 1];
            bool mk_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                int32_t kwid = mk_kw[r];
                for (int i = 0; i < kw_cnt; ++i) {
                    if (kwids_local[i] == kwid) { mk_ok = true; break; }
                }
                if (mk_ok) break;
            }
            if (!mk_ok) continue;

            // (4) mi_idx EXISTS with info_type_id==it2 AND info < "7.0"
            lo = mii_off[v]; hi = mii_off[v + 1];
            std::string_view local_miidx;
            bool miidx_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (mii_itid[r] != it2_id) continue;
                int64_t flo = mii_inf_off[r], fhi = mii_inf_off[r + 1];
                size_t flen = (size_t)(fhi - flo);
                const char* fdat = mii_inf_dat + flo;
                // info < "7.0"
                int cmp;
                if (flen < 3) {
                    cmp = std::memcmp(fdat, "7.0", flen);
                    if (cmp == 0) cmp = -1;  // shorter -> less
                } else {
                    cmp = std::memcmp(fdat, "7.0", 3);
                    if (cmp == 0 && flen > 3) cmp = 1;
                }
                if (cmp >= 0) continue;
                std::string_view cur(fdat, flen);
                if (!miidx_ok || cur < local_miidx) local_miidx = cur;
                miidx_ok = true;
            }
            if (!miidx_ok) continue;

            // (5) mi EXISTS with info_type_id==it1 AND info IN {Germany,German,USA,American}
            lo = mi_off[v]; hi = mi_off[v + 1];
            bool mi_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (mi_itid[r] != it1_id) continue;
                int64_t flo = mi_inf_off[r], fhi = mi_inf_off[r + 1];
                size_t flen = (size_t)(fhi - flo);
                const char* fdat = mi_inf_dat + flo;
                for (int j = 0; j < 4; ++j) {
                    if (flen == MI_INFO_LITERALS_LEN[j] &&
                        std::memcmp(fdat, MI_INFO_LITERALS[j], flen) == 0) {
                        mi_ok = true;
                        break;
                    }
                }
                if (mi_ok) break;
            }
            if (!mi_ok) continue;

            // (6) mc EXISTS: note NOT LIKE %(USA)% AND note LIKE %(200%)% AND cn.country_code != [us] && != NULL
            lo = mc_off[v]; hi = mc_off[v + 1];
            std::string_view local_cn;
            bool mc_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                int64_t nlo = mc_n_off[r], nhi = mc_n_off[r + 1];
                size_t nlen = (size_t)(nhi - nlo);
                if (nlen == 0) continue;  // empty note fails LIKE positive
                const char* ndat = mc_n_dat + nlo;
                // NOT LIKE %(USA)%
                if (contains_substr(ndat, nlen, "(USA)", 5)) continue;
                // LIKE %(200%)% — substring "(200"
                if (!contains_substr(ndat, nlen, "(200", 4)) continue;
                // company_id
                int32_t cid = mc_cid[r];
                if (cid <= 0) continue;
                int16_t cc = cn_cc[cid - 1];
                if (cc == 0) continue;          // NULL country_code
                if (cc == us_code) continue;    // [us]
                // OK, fetch cn.name
                int64_t clo = cn_n_off[cid - 1], chi = cn_n_off[cid];
                size_t clen = (size_t)(chi - clo);
                std::string_view cur(cn_n_dat + clo, clen);
                if (!mc_ok || cur < local_cn) local_cn = cur;
                mc_ok = true;
            }
            if (!mc_ok) continue;

            // All 4 EXISTS pass — update local mins
            int64_t tlo = t_off[row], thi = t_off[row + 1];
            std::string_view title_sv(t_dat + tlo, (size_t)(thi - tlo));
            if (!has) {
                best_t = title_sv; best_mi = local_miidx; best_cn = local_cn;
                has = true;
            } else {
                if (title_sv < best_t) best_t = title_sv;
                if (local_miidx < best_mi) best_mi = local_miidx;
                if (local_cn < best_cn) best_cn = local_cn;
            }
        }

        // store into thread-local mins (own the strings)
        if (has) {
            tmins[tid].has = true;
            tmins[tid].min_title.assign(best_t.data(), best_t.size());
            tmins[tid].min_miidx.assign(best_mi.data(), best_mi.size());
            tmins[tid].min_cn.assign(best_cn.data(), best_cn.size());
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        size_t chunk = (n_titles + n_threads - 1) / n_threads;
        for (int t = 0; t < n_threads; ++t) {
            size_t s = (size_t)t * chunk;
            size_t e = std::min(s + chunk, n_titles);
            if (s >= e) break;
            threads.emplace_back(worker, t, s, e);
        }
        for (auto& th : threads) th.join();
    }

    // --- Merge per-thread mins -------------------------------------------------
    std::string g_title, g_miidx, g_cn;
    bool g_has = false;
    for (auto& tm : tmins) {
        if (!tm.has) continue;
        if (!g_has) {
            g_title = tm.min_title; g_miidx = tm.min_miidx; g_cn = tm.min_cn;
            g_has = true;
        } else {
            if (tm.min_title < g_title) g_title = tm.min_title;
            if (tm.min_miidx < g_miidx) g_miidx = tm.min_miidx;
            if (tm.min_cn < g_cn) g_cn = tm.min_cn;
        }
    }

    // --- Output ----------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q22a.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_company,rating,western_violent_movie\n");
        if (g_has) {
            std::fprintf(f, "%.*s,%.*s,%.*s\n",
                         (int)g_cn.size(), g_cn.data(),
                         (int)g_miidx.size(), g_miidx.data(),
                         (int)g_title.size(), g_title.data());
        } else {
            std::fprintf(f, ",,\n");
        }
        std::fclose(f);
    }

    return 0;
}
