// Q23b: complete nerdy internet movie (2000s)
// MIN(kt.kind), MIN(t.title) over deep semi-join chain
#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

using namespace gendb;

// Tiny inline strstr replacement using memchr+memcmp for short needles
static inline bool contains_substr(const char* hay, size_t hl,
                                   const char* nd, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    const char first = nd[0];
    const char* p = hay;
    size_t remaining = hl;
    while (remaining >= nl) {
        const void* hit = std::memchr(p, first, remaining - nl + 1);
        if (!hit) return false;
        const char* hp = static_cast<const char*>(hit);
        if (std::memcmp(hp, nd, nl) == 0) return true;
        size_t advanced = (hp - p) + 1;
        p += advanced;
        remaining -= advanced;
    }
    return false;
}

static bool ensure_dir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0755) == 0;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    ensure_dir(results_dir);

    // ----- Phase 1: data loading & startup resolution -----
    int32_t cct1_id = -1;
    int16_t us_code = -1;
    int32_t it1_id  = -1;
    int32_t kt_id   = -1;
    std::string kt_kind_str = "movie";

    // kw_ids: 4 ids max
    int32_t kw_ids_arr[8];
    int kw_n = 0;

    // Mmap columns we need long-term
    MmapColumn<int32_t> t_id, t_kind_id, t_pyear;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> cc_off, mk_off, mc_off, mi_off;
    MmapColumn<int32_t> cc_status_id;
    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off, mi_note_off;
    MmapColumn<char>    mi_info_dat, mi_note_dat;

    {
        GENDB_PHASE("data_loading");

        // ---- resolve cct1_id from comp_cast_type ----
        {
            MmapColumn<int32_t> id_col(gendb_dir + "/comp_cast_type/id.bin");
            MmapColumn<int64_t> off(gendb_dir + "/comp_cast_type/kind.off");
            MmapColumn<char>    dat(gendb_dir + "/comp_cast_type/kind.dat");
            for (size_t i = 0; i < id_col.count; ++i) {
                int64_t a = off[i], b = off[i+1];
                size_t L = (size_t)(b - a);
                if (L == 17 && std::memcmp(dat.data + a, "complete+verified", 17) == 0) {
                    cct1_id = id_col[i];
                    break;
                }
            }
        }
        // ---- resolve it1_id from info_type ('release dates') ----
        {
            MmapColumn<int32_t> id_col(gendb_dir + "/info_type/id.bin");
            MmapColumn<int64_t> off(gendb_dir + "/info_type/info.off");
            MmapColumn<char>    dat(gendb_dir + "/info_type/info.dat");
            for (size_t i = 0; i < id_col.count; ++i) {
                int64_t a = off[i], b = off[i+1];
                size_t L = (size_t)(b - a);
                if (L == 13 && std::memcmp(dat.data + a, "release dates", 13) == 0) {
                    it1_id = id_col[i];
                    break;
                }
            }
        }
        // ---- resolve kt_id from kind_type ('movie') ----
        {
            MmapColumn<int32_t> id_col(gendb_dir + "/kind_type/id.bin");
            MmapColumn<int64_t> off(gendb_dir + "/kind_type/kind.off");
            MmapColumn<char>    dat(gendb_dir + "/kind_type/kind.dat");
            for (size_t i = 0; i < id_col.count; ++i) {
                int64_t a = off[i], b = off[i+1];
                size_t L = (size_t)(b - a);
                if (L == 5 && std::memcmp(dat.data + a, "movie", 5) == 0) {
                    kt_id = id_col[i];
                    kt_kind_str.assign(dat.data + a, L);
                    break;
                }
            }
        }
        // ---- resolve us_code dict code (country_code) ----
        {
            MmapColumn<int64_t> doff(gendb_dir + "/company_name/country_code.dict.off");
            MmapColumn<char>    ddat(gendb_dir + "/company_name/country_code.dict.dat");
            size_t nentries = doff.count > 0 ? doff.count - 1 : 0;
            for (size_t i = 0; i < nentries; ++i) {
                int64_t a = doff[i], b = doff[i+1];
                size_t L = (size_t)(b - a);
                if (L == 4 && std::memcmp(ddat.data + a, "[us]", 4) == 0) {
                    // dict codes are 1-based (0 = NULL sentinel)
                    us_code = (int16_t)(i + 1);
                    break;
                }
            }
        }
        // ---- resolve kw_ids ----
        {
            MmapColumn<int32_t> id_col(gendb_dir + "/keyword/id.bin");
            MmapColumn<int64_t> off(gendb_dir + "/keyword/keyword.off");
            MmapColumn<char>    dat(gendb_dir + "/keyword/keyword.dat");
            // Build needles
            struct N { const char* s; size_t len; };
            N needles[] = {
                {"nerd", 4}, {"loner", 5}, {"alienation", 10}, {"dignity", 7}
            };
            for (size_t i = 0; i < id_col.count && kw_n < 4; ++i) {
                int64_t a = off[i], b = off[i+1];
                size_t L = (size_t)(b - a);
                for (auto& nd : needles) {
                    if (L == nd.len && std::memcmp(dat.data + a, nd.s, nd.len) == 0) {
                        kw_ids_arr[kw_n++] = id_col[i];
                        break;
                    }
                }
            }
        }

        // ---- Open driver and fact columns ----
        t_id.open(gendb_dir + "/title/id.bin");
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_pyear.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        cc_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");

        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");
        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_note_off.open(gendb_dir + "/movie_info/note.off");
        mi_note_dat.open(gendb_dir + "/movie_info/note.dat");
    }

    if (cct1_id < 0 || it1_id < 0 || kt_id < 0 || us_code < 0 || kw_n == 0) {
        std::fprintf(stderr, "startup resolution failed: cct1=%d it1=%d kt=%d us=%d kw_n=%d\n",
                     cct1_id, it1_id, kt_id, (int)us_code, kw_n);
        // still write header
    }

    // ---- Main scan over title driver ----
    // Per-thread local MIN(title) tracked as title row index (uniqueness by .off/.dat).
    // We compare candidate t.title against current local best lazily.
    size_t n_titles = t_id.count;
    const int32_t* tid = t_id.data;
    const int32_t* tkind = t_kind_id.data;
    const int32_t* tpy = t_pyear.data;
    const int64_t* toff = t_title_off.data;
    const char*    tdat = t_title_dat.data;

    const int32_t* cc_off_d = cc_off.data;
    const int32_t* mk_off_d = mk_off.data;
    const int32_t* mc_off_d = mc_off.data;
    const int32_t* mi_off_d = mi_off.data;
    const int32_t* cc_st = cc_status_id.data;
    const int32_t* mk_kw = mk_keyword_id.data;
    const int32_t* mc_cid = mc_company_id.data;
    const int16_t* cn_cc = cn_country_code.data;
    const int32_t* mi_iti = mi_info_type_id.data;
    const int64_t* mi_io = mi_info_off.data;
    const char*    mi_id_ = mi_info_dat.data;
    const int64_t* mi_no = mi_note_off.data;
    const char*    mi_nd_ = mi_note_dat.data;

    size_t cc_off_count = cc_off.count;
    size_t mk_off_count = mk_off.count;
    size_t mc_off_count = mc_off.count;
    size_t mi_off_count = mi_off.count;

    // Capture kw_ids into local set (linear scan over 4 entries is faster than hashset)
    int32_t kws[4] = {0,0,0,0};
    for (int i = 0; i < kw_n; i++) kws[i] = kw_ids_arr[i];
    const int kw_count = kw_n;

    // Result MIN tracking
    std::string global_min_title;
    bool global_found = false;
    std::mutex result_mtx;

    {
        GENDB_PHASE("main_scan");

        unsigned nthreads = std::thread::hardware_concurrency();
        if (nthreads == 0) nthreads = 4;
        if (nthreads > 12) nthreads = 12;

        std::vector<std::thread> workers;
        std::vector<std::string> local_mins(nthreads);
        std::vector<bool> local_found(nthreads, false);

        size_t chunk = (n_titles + nthreads - 1) / nthreads;
        for (unsigned t = 0; t < nthreads; ++t) {
            size_t lo = t * chunk;
            size_t hi = std::min(lo + chunk, n_titles);
            workers.emplace_back([&, t, lo, hi]() {
                std::string best;
                bool found = false;

                for (size_t i = lo; i < hi; ++i) {
                    // Driver filters
                    if (tkind[i] != kt_id) continue;
                    if (tpy[i] <= 2000) continue;

                    int32_t mv = tid[i];
                    if (mv < 0) continue;

                    // ---- Semi-join 1: complete_cast (status_id == cct1_id) ----
                    if ((size_t)mv + 1 >= cc_off_count) continue;
                    int32_t cclo = cc_off_d[mv];
                    int32_t cchi = cc_off_d[mv + 1];
                    if (cchi <= cclo) continue;
                    {
                        bool ok = false;
                        for (int32_t r = cclo; r < cchi; ++r) {
                            if (cc_st[r] == cct1_id) { ok = true; break; }
                        }
                        if (!ok) continue;
                    }

                    // ---- Semi-join 2: movie_keyword (keyword_id IN kw_ids) ----
                    if ((size_t)mv + 1 >= mk_off_count) continue;
                    int32_t mklo = mk_off_d[mv];
                    int32_t mkhi = mk_off_d[mv + 1];
                    if (mkhi <= mklo) continue;
                    {
                        bool ok = false;
                        for (int32_t r = mklo; r < mkhi && !ok; ++r) {
                            int32_t k = mk_kw[r];
                            for (int j = 0; j < kw_count; ++j) {
                                if (k == kws[j]) { ok = true; break; }
                            }
                        }
                        if (!ok) continue;
                    }

                    // ---- Semi-join 3: movie_companies (country_code[company_id-1]==us_code) ----
                    if ((size_t)mv + 1 >= mc_off_count) continue;
                    int32_t mclo = mc_off_d[mv];
                    int32_t mchi = mc_off_d[mv + 1];
                    if (mchi <= mclo) continue;
                    {
                        bool ok = false;
                        for (int32_t r = mclo; r < mchi; ++r) {
                            int32_t cid = mc_cid[r];
                            if (cid >= 1 && cn_cc[cid - 1] == us_code) { ok = true; break; }
                        }
                        if (!ok) continue;
                    }

                    // ---- Semi-join 4: movie_info (info_type_id, note %internet%, info 'USA:% 200%') ----
                    if ((size_t)mv + 1 >= mi_off_count) continue;
                    int32_t milo = mi_off_d[mv];
                    int32_t mihi = mi_off_d[mv + 1];
                    if (mihi <= milo) continue;
                    {
                        bool ok = false;
                        for (int32_t r = milo; r < mihi; ++r) {
                            if (mi_iti[r] != it1_id) continue;
                            // note LIKE '%internet%'
                            int64_t na = mi_no[r], nb = mi_no[r + 1];
                            size_t nlen = (size_t)(nb - na);
                            if (nlen < 8) continue;
                            if (!contains_substr(mi_nd_ + na, nlen, "internet", 8)) continue;
                            // info startswith 'USA:'
                            int64_t ia = mi_io[r], ib = mi_io[r + 1];
                            size_t ilen = (size_t)(ib - ia);
                            if (ilen < 4) continue;
                            if (std::memcmp(mi_id_ + ia, "USA:", 4) != 0) continue;
                            // info contains ' 200'  (within remainder)
                            if (!contains_substr(mi_id_ + ia + 4, ilen - 4, " 200", 4)) continue;
                            ok = true; break;
                        }
                        if (!ok) continue;
                    }

                    // ---- Aggregate MIN(t.title) ----
                    int64_t ta = toff[i], tb = toff[i + 1];
                    size_t tlen = (size_t)(tb - ta);
                    const char* tp = tdat + ta;
                    // Compare to current best
                    bool replace;
                    if (!found) replace = true;
                    else {
                        size_t bl = best.size();
                        size_t ml = tlen < bl ? tlen : bl;
                        int c = std::memcmp(tp, best.data(), ml);
                        if (c < 0) replace = true;
                        else if (c == 0) replace = (tlen < bl);
                        else replace = false;
                    }
                    if (replace) {
                        best.assign(tp, tlen);
                        found = true;
                    }
                }
                local_mins[t] = std::move(best);
                local_found[t] = found;
            });
        }

        for (auto& w : workers) w.join();

        for (unsigned t = 0; t < nthreads; ++t) {
            if (!local_found[t]) continue;
            if (!global_found || local_mins[t] < global_min_title) {
                global_min_title = local_mins[t];
                global_found = true;
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q23b.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 2;
        }
        std::fprintf(fp, "movie_kind,complete_nerdy_internet_movie\n");
        if (global_found) {
            std::fprintf(fp, "%s,%s\n", kt_kind_str.c_str(), global_min_title.c_str());
        } else {
            std::fprintf(fp, ",\n");
        }
        std::fclose(fp);
    }

    return 0;
}
