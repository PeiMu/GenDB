// Q28c - JOB benchmark
// SELECT MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// Driver: title (production_year>2005, kind_id IN {movie,episode})
// Probes: mk(keyword in 4-set) → cc(subject=cast & status=complete)
//         → mi_idx(it2 & info<'8.5', MIN(info))
//         → mc(company in cn_set & note LIKE checks, MIN(cn.name))
//         → mi(it1 & info IN 10-set)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

// ------- helpers -------
static inline int sv_cmp(std::string_view a, std::string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c;
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return  1;
    return 0;
}

static inline bool has_substr(const char* s, size_t n, const char* needle, size_t m) {
    if (m > n) return false;
    const char first = needle[0];
    size_t lim = n - m;
    for (size_t i = 0; i <= lim; ++i) {
        if (s[i] == first && std::memcmp(s + i, needle, m) == 0) return true;
    }
    return false;
}

// note LIKE '%(200%)%' : need '(200' at some pos p, then ')' at some pos >= p+4
// note NOT LIKE '%(USA)%': must NOT contain '(USA)'
static inline bool note_matches(const char* s, size_t n) {
    // reject if contains "(USA)"
    static const char usa[] = "(USA)";
    if (has_substr(s, n, usa, 5)) return false;
    // find "(200"
    if (n < 5) return false; // need at least "(200X)" = 6, actually 5 chars min "(200)"
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (s[i] == '(' && s[i+1] == '2' && s[i+2] == '0' && s[i+3] == '0') {
            // scan for ')' at any later position
            for (size_t j = i + 4; j < n; ++j) {
                if (s[j] == ')') return true;
            }
            return false;
        }
    }
    return false;
}

// Read a varlen string at row i from off + dat
static inline std::string_view sv_at(const int64_t* off, const char* dat, int64_t i) {
    int64_t a = off[i], b = off[i+1];
    return std::string_view(dat + a, static_cast<size_t>(b - a));
}

int main(int argc, char* argv[]) {
    init_date_tables();
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];
    std::filesystem::create_directories(rdir);

    GENDB_PHASE("total");

    // ------- 1. Memory map columns -------
    MmapColumn<int32_t>  title_kind_id, title_py;
    MmapColumn<int64_t>  title_off;
    MmapColumn<char>     title_dat;

    MmapColumn<int32_t>  mk_keyword_id;
    MmapColumn<int32_t>  mk_off;       // index offsets indexed by movie_id

    MmapColumn<int32_t>  cc_subject_id, cc_status_id;
    MmapColumn<int32_t>  cc_off;

    MmapColumn<int32_t>  mi_idx_info_type_id;
    MmapColumn<int64_t>  mi_idx_info_off;
    MmapColumn<char>     mi_idx_info_dat;
    MmapColumn<int32_t>  mi_idx_off;

    MmapColumn<int32_t>  mc_company_id;
    MmapColumn<int64_t>  mc_note_off;
    MmapColumn<char>     mc_note_dat;
    MmapColumn<int32_t>  mc_off;

    MmapColumn<int32_t>  mi_info_type_id;
    MmapColumn<int64_t>  mi_info_off;
    MmapColumn<char>     mi_info_dat;
    MmapColumn<int32_t>  mi_off;

    // company_name
    MmapColumn<int64_t>  cn_name_off;
    MmapColumn<char>     cn_name_dat;
    MmapColumn<uint16_t> cn_country_code;
    MmapColumn<int64_t>  cn_dict_off;
    MmapColumn<char>     cn_dict_dat;

    // dim tables
    MmapColumn<int32_t>  kt_id;
    MmapColumn<int64_t>  kt_kind_off;
    MmapColumn<char>     kt_kind_dat;

    MmapColumn<int32_t>  it_id;
    MmapColumn<int64_t>  it_info_off;
    MmapColumn<char>     it_info_dat;

    MmapColumn<int32_t>  kw_id;
    MmapColumn<int64_t>  kw_off;
    MmapColumn<char>     kw_dat;

    MmapColumn<int32_t>  cct_id;
    MmapColumn<int64_t>  cct_kind_off;
    MmapColumn<char>     cct_kind_dat;

    {
        GENDB_PHASE("data_loading");

        title_kind_id.open(gdir + "/title/kind_id.bin");
        title_py.open    (gdir + "/title/production_year.bin");
        title_off.open   (gdir + "/title/title.off");
        title_dat.open   (gdir + "/title/title.dat");

        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");
        mk_off.open       (gdir + "/_idx/movie_keyword__movie_id__offsets.bin");

        cc_subject_id.open(gdir + "/complete_cast/subject_id.bin");
        cc_status_id.open (gdir + "/complete_cast/status_id.bin");
        cc_off.open       (gdir + "/_idx/complete_cast__movie_id__offsets.bin");

        mi_idx_info_type_id.open(gdir + "/movie_info_idx/info_type_id.bin");
        mi_idx_info_off.open    (gdir + "/movie_info_idx/info.off");
        mi_idx_info_dat.open    (gdir + "/movie_info_idx/info.dat");
        mi_idx_off.open         (gdir + "/_idx/movie_info_idx__movie_id__offsets.bin");

        mc_company_id.open(gdir + "/movie_companies/company_id.bin");
        mc_note_off.open  (gdir + "/movie_companies/note.off");
        mc_note_dat.open  (gdir + "/movie_companies/note.dat");
        mc_off.open       (gdir + "/_idx/movie_companies__movie_id__offsets.bin");

        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open    (gdir + "/movie_info/info.off");
        mi_info_dat.open    (gdir + "/movie_info/info.dat");
        mi_off.open         (gdir + "/_idx/movie_info__movie_id__offsets.bin");

        cn_name_off.open    (gdir + "/company_name/name.off");
        cn_name_dat.open    (gdir + "/company_name/name.dat");
        cn_country_code.open(gdir + "/company_name/country_code.bin");
        cn_dict_off.open    (gdir + "/company_name/country_code.dict.off");
        cn_dict_dat.open    (gdir + "/company_name/country_code.dict.dat");

        kt_id.open      (gdir + "/kind_type/id.bin");
        kt_kind_off.open(gdir + "/kind_type/kind.off");
        kt_kind_dat.open(gdir + "/kind_type/kind.dat");

        it_id.open      (gdir + "/info_type/id.bin");
        it_info_off.open(gdir + "/info_type/info.off");
        it_info_dat.open(gdir + "/info_type/info.dat");

        kw_id.open (gdir + "/keyword/id.bin");
        kw_off.open(gdir + "/keyword/keyword.off");
        kw_dat.open(gdir + "/keyword/keyword.dat");

        cct_id.open      (gdir + "/comp_cast_type/id.bin");
        cct_kind_off.open(gdir + "/comp_cast_type/kind.off");
        cct_kind_dat.open(gdir + "/comp_cast_type/kind.dat");

        // Prefetch large columns
        mi_info_type_id.prefetch();
        mi_off.prefetch();
        mk_keyword_id.prefetch();
        mk_off.prefetch();
        mc_company_id.prefetch();
        mc_note_off.prefetch();
        mc_off.prefetch();
        mi_idx_info_type_id.prefetch();
        mi_idx_off.prefetch();
        cc_subject_id.prefetch();
        cc_status_id.prefetch();
        cc_off.prefetch();
    }

    // ------- 2. Dim resolution -------
    int32_t cast_id = -1, complete_id = -1;
    int32_t it1_id = -1, it2_id = -1;
    uint16_t us_code = 0xFFFF;
    std::vector<int32_t> kw_list;
    std::vector<int32_t> kt_list;
    std::vector<bool> cn_set; // bitset by company_id (row index)

    {
        GENDB_PHASE("dim_resolution");

        // comp_cast_type
        for (size_t i = 0; i < cct_id.count; i++) {
            auto sv = sv_at(cct_kind_off.data, cct_kind_dat.data, (int64_t)i);
            if (sv == "cast")     cast_id = cct_id[i];
            if (sv == "complete") complete_id = cct_id[i];
        }
        // info_type
        for (size_t i = 0; i < it_id.count; i++) {
            auto sv = sv_at(it_info_off.data, it_info_dat.data, (int64_t)i);
            if (sv == "countries") it1_id = it_id[i];
            if (sv == "rating")    it2_id = it_id[i];
        }
        // kind_type
        for (size_t i = 0; i < kt_id.count; i++) {
            auto sv = sv_at(kt_kind_off.data, kt_kind_dat.data, (int64_t)i);
            if (sv == "movie" || sv == "episode") kt_list.push_back(kt_id[i]);
        }
        // keyword
        for (size_t i = 0; i < kw_id.count; i++) {
            auto sv = sv_at(kw_off.data, kw_dat.data, (int64_t)i);
            if (sv == "murder" || sv == "murder-in-title" ||
                sv == "blood"  || sv == "violence") {
                kw_list.push_back(kw_id[i]);
            }
        }
        // company_name dict + cn_set
        // find us_code
        size_t n_dict = cn_dict_off.count - 1;
        for (size_t i = 0; i < n_dict; i++) {
            auto sv = sv_at(cn_dict_off.data, cn_dict_dat.data, (int64_t)i);
            if (sv == "[us]") { us_code = (uint16_t)i; break; }
        }
        cn_set.assign(cn_country_code.count, false);
        for (size_t i = 0; i < cn_country_code.count; i++) {
            if (cn_country_code[i] != us_code) cn_set[i] = true;
        }
    }

    if (cast_id < 0 || complete_id < 0 || it1_id < 0 || it2_id < 0 ||
        kw_list.empty() || kt_list.empty() || us_code == 0xFFFF) {
        std::fprintf(stderr, "dim resolution failed\n");
        return 1;
    }

    // mi.info 10-literal set: linear lookup is fine for 10 items
    static const std::string_view MI_INFO_SET[] = {
        std::string_view("Sweden"),    std::string_view("Norway"),
        std::string_view("Germany"),   std::string_view("Denmark"),
        std::string_view("Swedish"),   std::string_view("Danish"),
        std::string_view("Norwegian"), std::string_view("German"),
        std::string_view("USA"),       std::string_view("American")
    };
    const int MI_INFO_N = 10;

    // ------- 3. Driver scan: filter title by py>2005 and kind_id IN kt_set -------
    std::vector<int32_t> t_ids;
    {
        GENDB_PHASE("driver_scan");
        // Single thread is fine: 2.5M ints fast
        size_t n = title_kind_id.count;
        t_ids.reserve(n / 2);
        int32_t k0 = kt_list[0];
        int32_t k1 = kt_list.size() > 1 ? kt_list[1] : -1;
        for (size_t i = 0; i < n; ++i) {
            int32_t ki = title_kind_id[i];
            int32_t py = title_py[i];
            if (py > 2005 && (ki == k0 || ki == k1)) {
                // title.id == i + 1
                t_ids.push_back((int32_t)(i + 1));
            }
        }
    }

    // ------- 4. Parallel main scan -------
    // Per-thread MIN trio (pointer + length into mmaped .dat)
    struct MinSV {
        const char* p = nullptr;
        size_t      n = 0;
        bool set = false;
        inline void update(std::string_view v) {
            if (!set) { p = v.data(); n = v.size(); set = true; return; }
            int c = sv_cmp(std::string_view(p, n), v);
            if (c > 0) { p = v.data(); n = v.size(); }
        }
    };

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > 12) nthreads = 12;

    std::vector<MinSV> tl_cn(nthreads), tl_rat(nthreads), tl_title(nthreads);

    {
        GENDB_PHASE("main_scan");

        std::vector<std::thread> workers;
        size_t total = t_ids.size();
        const size_t MORSEL = 4096;
        std::atomic<size_t> next{0};

        auto worker = [&](unsigned tid) {
            MinSV& bcn = tl_cn[tid];
            MinSV& brat = tl_rat[tid];
            MinSV& btitle = tl_title[tid];

            const int32_t* mk_off_p = mk_off.data;
            const int32_t* mk_kw_p  = mk_keyword_id.data;
            const int32_t* cc_off_p = cc_off.data;
            const int32_t* cc_sub_p = cc_subject_id.data;
            const int32_t* cc_sta_p = cc_status_id.data;
            const int32_t* mi_idx_off_p = mi_idx_off.data;
            const int32_t* mi_idx_it_p  = mi_idx_info_type_id.data;
            const int64_t* mi_idx_io_p  = mi_idx_info_off.data;
            const char*    mi_idx_id_p  = mi_idx_info_dat.data;
            const int32_t* mc_off_p     = mc_off.data;
            const int32_t* mc_co_p      = mc_company_id.data;
            const int64_t* mc_no_p      = mc_note_off.data;
            const char*    mc_nd_p      = mc_note_dat.data;
            const int32_t* mi_off_p     = mi_off.data;
            const int32_t* mi_it_p      = mi_info_type_id.data;
            const int64_t* mi_io_p      = mi_info_off.data;
            const char*    mi_id_p      = mi_info_dat.data;

            // mk index size minus 1
            // offsets are dimensioned to max_movie_id + 1
            size_t mk_off_max = mk_off.count > 0 ? mk_off.count - 1 : 0;
            size_t cc_off_max = cc_off.count > 0 ? cc_off.count - 1 : 0;
            size_t mi_idx_off_max = mi_idx_off.count > 0 ? mi_idx_off.count - 1 : 0;
            size_t mc_off_max = mc_off.count > 0 ? mc_off.count - 1 : 0;
            size_t mi_off_max = mi_off.count > 0 ? mi_off.count - 1 : 0;

            const int32_t kw0 = kw_list.size() > 0 ? kw_list[0] : -1;
            const int32_t kw1 = kw_list.size() > 1 ? kw_list[1] : -1;
            const int32_t kw2 = kw_list.size() > 2 ? kw_list[2] : -1;
            const int32_t kw3 = kw_list.size() > 3 ? kw_list[3] : -1;

            while (true) {
                size_t start = next.fetch_add(MORSEL, std::memory_order_relaxed);
                if (start >= total) break;
                size_t end = std::min(start + MORSEL, total);

                for (size_t i = start; i < end; ++i) {
                    int32_t tid_v = t_ids[i];

                    // -- mk semi-join: any keyword in kw_list --
                    if ((size_t)tid_v >= mk_off_max) continue;
                    int32_t a = mk_off_p[tid_v];
                    int32_t b = mk_off_p[tid_v + 1];
                    bool ok = false;
                    for (int32_t r = a; r < b; ++r) {
                        int32_t k = mk_kw_p[r];
                        if (k == kw0 || k == kw1 || k == kw2 || k == kw3) { ok = true; break; }
                    }
                    if (!ok) continue;

                    // -- cc semi-join: subject=cast & status=complete --
                    if ((size_t)tid_v >= cc_off_max) continue;
                    a = cc_off_p[tid_v];
                    b = cc_off_p[tid_v + 1];
                    ok = false;
                    for (int32_t r = a; r < b; ++r) {
                        if (cc_sub_p[r] == cast_id && cc_sta_p[r] == complete_id) {
                            ok = true; break;
                        }
                    }
                    if (!ok) continue;

                    // -- mi_idx inner: info_type==it2 & info<'8.5'; collect MIN(info) --
                    if ((size_t)tid_v >= mi_idx_off_max) continue;
                    a = mi_idx_off_p[tid_v];
                    b = mi_idx_off_p[tid_v + 1];
                    bool mi_idx_found = false;
                    std::string_view best_rating;
                    for (int32_t r = a; r < b; ++r) {
                        if (mi_idx_it_p[r] != it2_id) continue;
                        int64_t io = mi_idx_io_p[r];
                        int64_t ie = mi_idx_io_p[r + 1];
                        std::string_view v(mi_idx_id_p + io, (size_t)(ie - io));
                        // info < '8.5'
                        if (sv_cmp(v, std::string_view("8.5", 3)) >= 0) continue;
                        if (!mi_idx_found || sv_cmp(best_rating, v) > 0) {
                            best_rating = v;
                            mi_idx_found = true;
                        }
                    }
                    if (!mi_idx_found) continue;

                    // -- mc inner: company_id in cn_set & note checks; collect MIN(cn.name) --
                    if ((size_t)tid_v >= mc_off_max) continue;
                    a = mc_off_p[tid_v];
                    b = mc_off_p[tid_v + 1];
                    bool mc_found = false;
                    std::string_view best_cn;
                    for (int32_t r = a; r < b; ++r) {
                        int32_t cid = mc_co_p[r];
                        if (cid < 1 || (size_t)cid > cn_set.size()) continue;
                        if (!cn_set[(size_t)(cid - 1)]) continue;
                        // note check: must be present and pass LIKE conditions
                        int64_t no = mc_no_p[r];
                        int64_t ne = mc_no_p[r + 1];
                        size_t nlen = (size_t)(ne - no);
                        if (nlen == 0) continue; // NULL note doesn't match LIKE
                        if (!note_matches(mc_nd_p + no, nlen)) continue;
                        // look up cn.name[cid-1]
                        int64_t co = cn_name_off.data[cid - 1];
                        int64_t ce = cn_name_off.data[cid];
                        std::string_view nm(cn_name_dat.data + co, (size_t)(ce - co));
                        if (!mc_found || sv_cmp(best_cn, nm) > 0) {
                            best_cn = nm;
                            mc_found = true;
                        }
                    }
                    if (!mc_found) continue;

                    // -- mi semi-join: info_type==it1 & info IN 10-set --
                    if ((size_t)tid_v >= mi_off_max) continue;
                    a = mi_off_p[tid_v];
                    b = mi_off_p[tid_v + 1];
                    ok = false;
                    for (int32_t r = a; r < b; ++r) {
                        if (mi_it_p[r] != it1_id) continue;
                        int64_t io = mi_io_p[r];
                        int64_t ie = mi_io_p[r + 1];
                        std::string_view v(mi_id_p + io, (size_t)(ie - io));
                        for (int k = 0; k < MI_INFO_N; k++) {
                            if (v == MI_INFO_SET[k]) { ok = true; break; }
                        }
                        if (ok) break;
                    }
                    if (!ok) continue;

                    // -- All filters pass: update local MINs --
                    bcn.update(best_cn);
                    brat.update(best_rating);
                    // t.title at row tid_v - 1
                    int64_t to = title_off.data[tid_v - 1];
                    int64_t te = title_off.data[tid_v];
                    btitle.update(std::string_view(title_dat.data + to, (size_t)(te - to)));
                }
            }
        };

        for (unsigned t = 0; t < nthreads; ++t) workers.emplace_back(worker, t);
        for (auto& w : workers) w.join();
    }

    // ------- 5. Reduce -------
    MinSV gcn, grat, gtitle;
    for (unsigned t = 0; t < nthreads; t++) {
        if (tl_cn[t].set)    gcn.update   (std::string_view(tl_cn[t].p, tl_cn[t].n));
        if (tl_rat[t].set)   grat.update  (std::string_view(tl_rat[t].p, tl_rat[t].n));
        if (tl_title[t].set) gtitle.update(std::string_view(tl_title[t].p, tl_title[t].n));
    }

    // ------- 6. Output CSV -------
    {
        GENDB_PHASE("output");
        std::string outpath = rdir + "/Q28c.csv";
        FILE* f = std::fopen(outpath.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 1; }
        std::fprintf(f, "movie_company,rating,complete_euro_dark_movie\n");
        if (gcn.set) std::fwrite(gcn.p, 1, gcn.n, f);
        std::fputc(',', f);
        if (grat.set) std::fwrite(grat.p, 1, grat.n, f);
        std::fputc(',', f);
        if (gtitle.set) std::fwrite(gtitle.p, 1, gtitle.n, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    return 0;
}
