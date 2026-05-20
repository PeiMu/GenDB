// Q22c — JOB benchmark
// Plan: drive title with cheap int filters; per-title probe mk/mi semi, mi_idx + mc inner.
// Track per-thread MIN(cn.name), MIN(mi_idx.info), MIN(t.title); combine across threads.

#define _GNU_SOURCE
#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>
#include <omp.h>

using namespace gendb;

static inline bool has_substr(const char* h, size_t hn, const char* n, size_t nn) {
    if (nn > hn) return false;
    return ::memmem(h, hn, n, nn) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: q22c <storage> <results>\n");
        return 1;
    }
    std::string store   = argv[1];
    std::string results = argv[2];
    mkdir(results.c_str(), 0755);

    // ---- mmap columns ----
    MmapColumn<int32_t> t_id, t_kind_id, t_production_year;
    MmapColumn<int64_t> t_title_off;   MmapColumn<char> t_title_dat;

    MmapColumn<int32_t> kt_id;
    MmapColumn<int64_t> kt_kind_off;   MmapColumn<char> kt_kind_dat;

    MmapColumn<int32_t> it_id;
    MmapColumn<int64_t> it_info_off;   MmapColumn<char> it_info_dat;

    MmapColumn<int32_t> kw_id;
    MmapColumn<int64_t> kw_kw_off;     MmapColumn<char> kw_kw_dat;

    MmapColumn<int32_t> cn_id;
    MmapColumn<int64_t> cn_name_off;   MmapColumn<char> cn_name_dat;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_cc_dict_off;MmapColumn<char> cn_cc_dict_dat;

    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mk_off_mid;

    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;   MmapColumn<char> mi_info_dat;
    MmapColumn<int32_t> mi_off_mid;

    MmapColumn<int32_t> mx_info_type_id;
    MmapColumn<int64_t> mx_info_off;   MmapColumn<char> mx_info_dat;
    MmapColumn<int32_t> mx_off_mid;

    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int64_t> mc_note_off;   MmapColumn<char> mc_note_dat;
    MmapColumn<int32_t> mc_off_mid;

    {
        GENDB_PHASE("data_loading");
        t_id.open(store + "/title/id.bin");
        t_kind_id.open(store + "/title/kind_id.bin");
        t_production_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        kt_id.open(store + "/kind_type/id.bin");
        kt_kind_off.open(store + "/kind_type/kind.off");
        kt_kind_dat.open(store + "/kind_type/kind.dat");

        it_id.open(store + "/info_type/id.bin");
        it_info_off.open(store + "/info_type/info.off");
        it_info_dat.open(store + "/info_type/info.dat");

        kw_id.open(store + "/keyword/id.bin");
        kw_kw_off.open(store + "/keyword/keyword.off");
        kw_kw_dat.open(store + "/keyword/keyword.dat");

        cn_id.open(store + "/company_name/id.bin");
        cn_name_off.open(store + "/company_name/name.off");
        cn_name_dat.open(store + "/company_name/name.dat");
        cn_cc.open(store + "/company_name/country_code.bin");
        cn_cc_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(store + "/company_name/country_code.dict.dat");

        mk_keyword_id.open(store + "/movie_keyword/keyword_id.bin");
        mk_off_mid.open(store + "/_idx/movie_keyword__movie_id__offsets.bin");

        mi_info_type_id.open(store + "/movie_info/info_type_id.bin");
        mi_info_off.open(store + "/movie_info/info.off");
        mi_info_dat.open(store + "/movie_info/info.dat");
        mi_off_mid.open(store + "/_idx/movie_info__movie_id__offsets.bin");

        mx_info_type_id.open(store + "/movie_info_idx/info_type_id.bin");
        mx_info_off.open(store + "/movie_info_idx/info.off");
        mx_info_dat.open(store + "/movie_info_idx/info.dat");
        mx_off_mid.open(store + "/_idx/movie_info_idx__movie_id__offsets.bin");

        mc_company_id.open(store + "/movie_companies/company_id.bin");
        mc_note_off.open(store + "/movie_companies/note.off");
        mc_note_dat.open(store + "/movie_companies/note.dat");
        mc_off_mid.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
    }

    // ---- Resolve dim IDs ----
    int32_t it1_id = -1, it2_id = -1;
    int32_t kt_ids[2] = {-1, -1};
    int n_kt = 0;
    std::unordered_set<int32_t> kw_ids;
    int16_t us_code = -1;
    {
        GENDB_PHASE("resolve_dims");
        for (size_t i = 0; i < it_id.size(); i++) {
            auto a = it_info_off[i], b = it_info_off[i+1];
            std::string_view s(it_info_dat.data + a, b - a);
            if (s == "countries") it1_id = it_id[i];
            else if (s == "rating") it2_id = it_id[i];
        }
        for (size_t i = 0; i < kt_id.size(); i++) {
            auto a = kt_kind_off[i], b = kt_kind_off[i+1];
            std::string_view s(kt_kind_dat.data + a, b - a);
            if (s == "movie" || s == "episode") {
                if (n_kt < 2) kt_ids[n_kt++] = kt_id[i];
            }
        }
        kw_ids.reserve(8);
        for (size_t i = 0; i < kw_id.size(); i++) {
            auto a = kw_kw_off[i], b = kw_kw_off[i+1];
            std::string_view s(kw_kw_dat.data + a, b - a);
            if (s == "murder" || s == "murder-in-title" ||
                s == "blood"  || s == "violence")
                kw_ids.insert(kw_id[i]);
        }
        size_t n_cc = cn_cc_dict_off.size() - 1;
        for (size_t i = 0; i < n_cc; i++) {
            auto a = cn_cc_dict_off[i], b = cn_cc_dict_off[i+1];
            std::string_view s(cn_cc_dict_dat.data + a, b - a);
            if (s == "[us]") { us_code = (int16_t)i; break; }
        }
    }

    // ---- cn_ok bitset (country_code != 0 && != us_code) ----
    const size_t cn_n = cn_id.size();
    std::vector<uint64_t> cn_ok(cn_n / 64 + 1, 0);
    {
        GENDB_PHASE("build_cn_ok");
        for (size_t i = 0; i < cn_n; i++) {
            int16_t cc = cn_cc[i];
            if (cc != 0 && cc != us_code) {
                cn_ok[i >> 6] |= (uint64_t)1 << (i & 63);
            }
        }
    }
    auto cn_test = [&](int32_t cid) -> bool {
        size_t r = (size_t)(cid - 1);
        if (r >= cn_n) return false;
        return ((cn_ok[r >> 6] >> (r & 63)) & 1ULL) != 0ULL;
    };

    // ---- mi info set ----
    std::unordered_set<std::string_view> mi_info_set;
    mi_info_set.reserve(16);
    mi_info_set.insert("Sweden");
    mi_info_set.insert("Norway");
    mi_info_set.insert("Germany");
    mi_info_set.insert("Denmark");
    mi_info_set.insert("Swedish");
    mi_info_set.insert("Danish");
    mi_info_set.insert("Norwegian");
    mi_info_set.insert("German");
    mi_info_set.insert("USA");
    mi_info_set.insert("American");

    // ---- Projection helpers ----
    auto cn_name_sv = [&](int32_t cid) -> std::string_view {
        size_t r = (size_t)(cid - 1);
        auto a = cn_name_off[r], b = cn_name_off[r+1];
        return std::string_view(cn_name_dat.data + a, b - a);
    };
    auto title_sv = [&](int32_t mv) -> std::string_view {
        size_t r = (size_t)(mv - 1);
        auto a = t_title_off[r], b = t_title_off[r+1];
        return std::string_view(t_title_dat.data + a, b - a);
    };
    auto mxinfo_sv = [&](int32_t r) -> std::string_view {
        auto a = mx_info_off[r], b = mx_info_off[r+1];
        return std::string_view(mx_info_dat.data + a, b - a);
    };

    const size_t title_n = t_kind_id.size();

    int nthreads_max = omp_get_max_threads();
    std::vector<int32_t> best_cn_id(nthreads_max, -1);
    std::vector<int32_t> best_mx_row(nthreads_max, -1);
    std::vector<int32_t> best_title_id(nthreads_max, -1);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            int32_t lb_cn = -1;
            int32_t lb_mx = -1;
            int32_t lb_tt = -1;

            #pragma omp for schedule(dynamic, 65536) nowait
            for (size_t v = 0; v < title_n; v++) {
                int32_t py = t_production_year[v];
                if (py == INT32_MIN || py <= 2005) continue;
                int32_t kid = t_kind_id[v];
                bool kt_match = false;
                for (int i = 0; i < n_kt; i++) {
                    if (kt_ids[i] == kid) { kt_match = true; break; }
                }
                if (!kt_match) continue;

                int32_t mv = (int32_t)(v + 1);

                // ---- mk semi-join ----
                int32_t mk_lo = mk_off_mid[v], mk_hi = mk_off_mid[v + 1];
                bool mk_ok = false;
                for (int32_t r = mk_lo; r < mk_hi; r++) {
                    if (kw_ids.find(mk_keyword_id[r]) != kw_ids.end()) {
                        mk_ok = true; break;
                    }
                }
                if (!mk_ok) continue;

                // ---- mi semi-join ----
                int32_t mi_lo = mi_off_mid[v], mi_hi = mi_off_mid[v + 1];
                bool mi_ok = false;
                for (int32_t r = mi_lo; r < mi_hi; r++) {
                    if (mi_info_type_id[r] != it1_id) continue;
                    auto a = mi_info_off[r], b = mi_info_off[r+1];
                    size_t len = (size_t)(b - a);
                    if (len < 3 || len > 10) continue;
                    std::string_view s(mi_info_dat.data + a, len);
                    if (mi_info_set.find(s) != mi_info_set.end()) {
                        mi_ok = true; break;
                    }
                }
                if (!mi_ok) continue;

                // ---- mi_idx inner: need at least one with info<"8.5"; track local min info ----
                int32_t mx_lo = mx_off_mid[v], mx_hi = mx_off_mid[v + 1];
                int32_t local_mx = -1;
                for (int32_t r = mx_lo; r < mx_hi; r++) {
                    if (mx_info_type_id[r] != it2_id) continue;
                    auto a = mx_info_off[r], b = mx_info_off[r+1];
                    size_t len = (size_t)(b - a);
                    const char* p = mx_info_dat.data + a;
                    // lex compare info < "8.5"
                    size_t cmp_len = len < 3 ? len : 3;
                    int c = std::memcmp(p, "8.5", cmp_len);
                    bool less;
                    if (c < 0) less = true;
                    else if (c > 0) less = false;
                    else less = (len < 3);
                    if (!less) continue;
                    if (local_mx < 0) {
                        local_mx = r;
                    } else {
                        auto av = mx_info_off[local_mx], bv = mx_info_off[local_mx+1];
                        std::string_view cur(mx_info_dat.data + av, (size_t)(bv - av));
                        std::string_view ns(p, len);
                        if (ns < cur) local_mx = r;
                    }
                }
                if (local_mx < 0) continue;

                // ---- mc inner: track local min(cn.name) ----
                int32_t mc_lo = mc_off_mid[v], mc_hi = mc_off_mid[v + 1];
                int32_t local_cn = -1;
                for (int32_t r = mc_lo; r < mc_hi; r++) {
                    auto na = mc_note_off[r], nb = mc_note_off[r+1];
                    size_t nlen = (size_t)(nb - na);
                    if (nlen == 0) continue;
                    const char* nptr = mc_note_dat.data + na;
                    if (!has_substr(nptr, nlen, "(200", 4)) continue;
                    if (has_substr(nptr, nlen, "(USA)", 5)) continue;
                    int32_t cid = mc_company_id[r];
                    if (!cn_test(cid)) continue;
                    if (local_cn < 0) local_cn = cid;
                    else if (cn_name_sv(cid) < cn_name_sv(local_cn)) local_cn = cid;
                }
                if (local_cn < 0) continue;

                // ---- Update thread-local mins ----
                if (lb_cn < 0 || cn_name_sv(local_cn) < cn_name_sv(lb_cn)) lb_cn = local_cn;
                if (lb_mx < 0 || mxinfo_sv(local_mx) < mxinfo_sv(lb_mx)) lb_mx = local_mx;
                if (lb_tt < 0 || title_sv(mv) < title_sv(lb_tt)) lb_tt = mv;
            }

            best_cn_id[tid]    = lb_cn;
            best_mx_row[tid]   = lb_mx;
            best_title_id[tid] = lb_tt;
        }
    }

    // ---- Reduce across threads ----
    int32_t fin_cn = -1, fin_mx = -1, fin_tt = -1;
    for (int i = 0; i < nthreads_max; i++) {
        if (best_cn_id[i] >= 0) {
            if (fin_cn < 0 || cn_name_sv(best_cn_id[i]) < cn_name_sv(fin_cn))
                fin_cn = best_cn_id[i];
        }
        if (best_mx_row[i] >= 0) {
            if (fin_mx < 0 || mxinfo_sv(best_mx_row[i]) < mxinfo_sv(fin_mx))
                fin_mx = best_mx_row[i];
        }
        if (best_title_id[i] >= 0) {
            if (fin_tt < 0 || title_sv(best_title_id[i]) < title_sv(fin_tt))
                fin_tt = best_title_id[i];
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q22c.csv";
        FILE* f = fopen(outpath.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 1; }
        fprintf(f, "movie_company,rating,western_violent_movie\n");
        if (fin_cn > 0 && fin_mx >= 0 && fin_tt > 0) {
            auto cn_s = cn_name_sv(fin_cn);
            auto mx_s = mxinfo_sv(fin_mx);
            auto tt_s = title_sv(fin_tt);
            fwrite(cn_s.data(), 1, cn_s.size(), f);
            fputc(',', f);
            fwrite(mx_s.data(), 1, mx_s.size(), f);
            fputc(',', f);
            fwrite(tt_s.data(), 1, tt_s.size(), f);
            fputc('\n', f);
        }
        fclose(f);
    }

    (void)argc; (void)argv;
    return 0;
}
