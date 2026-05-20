// Q14c - IMDB JOB
// SELECT MIN(mi_idx.info) AS rating,
//        MIN(t.title) AS north_european_dark_production
// FROM info_type it1, info_type it2, keyword k, kind_type kt,
//      movie_info mi, movie_info_idx mi_idx, movie_keyword mk, title t
// WHERE it1.info='countries' AND it2.info='rating'
//   AND k.keyword IN ('murder','murder-in-title','blood','violence')
//   AND kt.kind IN ('movie','episode')
//   AND mi.info IN (10 countries)
//   AND mi_idx.info < '8.5'
//   AND t.production_year > 2005
//   AND joins on movie_id/info_type_id/keyword_id/kind_id
//
// Strategy (matching plan): title-driven star scan.
//   1. Resolve it1_id, it2_id, kt_ids[2], k_ids[<=4], country_info_set up front.
//   2. Per title r (parallel, morsel-driven):
//        a. py>2005 && kind_id in kt_ids
//        b. probe mk[mkmid[id]..mkmid[id+1]) for keyword_id in k_ids
//        c. probe mi[mimid[id]..mimid[id+1]) for info_type_id==it1_id && info in country_set
//        d. scan mi_idx[..] for info_type_id==it2_id && lex_lt(info,'8.5'); update MINs
//   3. Output min(mi_idx.info), min(t.title).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <sys/stat.h>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static inline int lex_cmp(const char* a, size_t la, const char* b, size_t lb) {
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

// lex_lt(a, "8.5")
static inline bool lex_lt_85(const char* a, size_t la) {
    static const char ref[3] = {'8','.','5'};
    size_t m = la < 3 ? la : 3;
    int c = std::memcmp(a, ref, m);
    if (c != 0) return c < 0;
    return la < 3;
}

struct StrBuf {
    std::vector<char> buf;
    bool has = false;
    void set(const char* p, size_t n) { buf.assign(p, p + n); has = true; }
    void try_min(const char* p, size_t n) {
        if (!has) { set(p, n); return; }
        if (lex_cmp(p, n, buf.data(), buf.size()) < 0) set(p, n);
    }
    void try_min(const StrBuf& o) {
        if (!o.has) return;
        if (!has) { buf = o.buf; has = true; return; }
        if (lex_cmp(o.buf.data(), o.buf.size(), buf.data(), buf.size()) < 0) buf = o.buf;
    }
};

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // --- Load (mmap) columns/indexes ---
    MmapColumn<int64_t> title_title_off, it_info_off, kw_off, kt_off, mi_info_off, mii_info_off;
    MmapColumn<char>    title_title_dat, it_info_dat, kw_dat, kt_dat, mi_info_dat, mii_info_dat;
    MmapColumn<int32_t> title_kind_id, title_py;
    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mi_info_type_id, mii_info_type_id;
    MmapColumn<int32_t> mk_off_idx, mi_off_idx, mii_off_idx;

    {
        GENDB_PHASE("data_loading");
        title_title_off.open(gendb_dir + "/title/title.off");
        title_title_dat.open(gendb_dir + "/title/title.dat");
        title_kind_id.open(gendb_dir + "/title/kind_id.bin");
        title_py.open(gendb_dir + "/title/production_year.bin");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        kt_off.open(gendb_dir + "/kind_type/kind.off");
        kt_dat.open(gendb_dir + "/kind_type/kind.dat");

        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        mii_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mii_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        mii_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        mk_off_idx.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mii_off_idx.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
    }

    // --- Resolve dim literals ---
    int32_t it1_id = -1, it2_id = -1;
    int32_t kt_ids[2] = {-1, -1};
    int kt_n = 0;
    int32_t k_ids[8];
    int k_n = 0;

    {
        GENDB_PHASE("resolve_dims");
        // info_type: 'countries' -> it1_id, 'rating' -> it2_id
        {
            size_t n = it_info_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = it_info_off.data[i], e = it_info_off.data[i+1];
                size_t l = (size_t)(e - s);
                const char* p = it_info_dat.data + s;
                if (l == 9 && std::memcmp(p, "countries", 9) == 0) it1_id = (int32_t)(i + 1);
                else if (l == 6 && std::memcmp(p, "rating", 6) == 0) it2_id = (int32_t)(i + 1);
            }
        }
        // kind_type: 'movie','episode'
        {
            size_t n = kt_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = kt_off.data[i], e = kt_off.data[i+1];
                size_t l = (size_t)(e - s);
                const char* p = kt_dat.data + s;
                if (l == 5 && std::memcmp(p, "movie", 5) == 0) {
                    kt_ids[kt_n++] = (int32_t)(i + 1);
                } else if (l == 7 && std::memcmp(p, "episode", 7) == 0) {
                    kt_ids[kt_n++] = (int32_t)(i + 1);
                }
            }
        }
        // keyword: 'murder','murder-in-title','blood','violence'
        {
            static const char* kws[4]   = {"murder","murder-in-title","blood","violence"};
            static const size_t kwlen[4] = {6, 15, 5, 8};
            size_t n = kw_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = kw_off.data[i], e = kw_off.data[i+1];
                size_t l = (size_t)(e - s);
                const char* p = kw_dat.data + s;
                for (int j = 0; j < 4; ++j) {
                    if (l == kwlen[j] && std::memcmp(p, kws[j], l) == 0) {
                        k_ids[k_n++] = (int32_t)(i + 1);
                        break;
                    }
                }
            }
        }
    }

    if (it1_id < 0 || it2_id < 0 || kt_n == 0 || k_n == 0) {
        std::fprintf(stderr, "Dim resolution failed: it1=%d it2=%d kt_n=%d k_n=%d\n",
                     it1_id, it2_id, kt_n, k_n);
        // Emit empty result still (NULLs)
        std::string out_path = results_dir + "/Q14c.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (f) {
            std::fprintf(f, "rating,north_european_dark_production\n,\n");
            std::fclose(f);
        }
        return 0;
    }

    // Country set (10 strings).
    // Use a simple flat array of {ptr,len} for fast equality check.
    struct StrLit { const char* p; size_t l; };
    static const StrLit countries[10] = {
        {"Sweden", 6}, {"Norway", 6}, {"Germany", 7}, {"Denmark", 7},
        {"Swedish", 7}, {"Danish", 6}, {"Norwegian", 9}, {"German", 6},
        {"USA", 3}, {"American", 8}
    };

    auto is_country = [&](const char* s, size_t l) -> bool {
        // Quick branch by length first
        switch (l) {
            case 3: return std::memcmp(s, "USA", 3) == 0;
            case 6:
                return std::memcmp(s,"Sweden",6)==0 ||
                       std::memcmp(s,"Norway",6)==0 ||
                       std::memcmp(s,"Danish",6)==0 ||
                       std::memcmp(s,"German",6)==0;
            case 7:
                return std::memcmp(s,"Germany",7)==0 ||
                       std::memcmp(s,"Denmark",7)==0 ||
                       std::memcmp(s,"Swedish",7)==0;
            case 8:
                return std::memcmp(s,"American",8)==0;
            case 9:
                return std::memcmp(s,"Norwegian",9)==0;
            default: return false;
        }
        (void)countries;
    };

    // --- Parallel scan ---
    size_t title_rows = title_py.count;
    int nthreads = (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 12) nthreads = 12;

    std::vector<StrBuf> th_min_rating(nthreads), th_min_title(nthreads);

    {
        GENDB_PHASE("main_scan");

        const int32_t* py = title_py.data;
        const int32_t* kid_col = title_kind_id.data;
        const int32_t* mk_off = mk_off_idx.data;
        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* mii_off = mii_off_idx.data;
        const int32_t* mk_kw = mk_keyword_id.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int64_t* mi_io = mi_info_off.data;
        const char*    mi_id = mi_info_dat.data;
        const int32_t* mii_itid = mii_info_type_id.data;
        const int64_t* mii_io = mii_info_off.data;
        const char*    mii_id = mii_info_dat.data;
        const int64_t* t_to = title_title_off.data;
        const char*    t_td = title_title_dat.data;

        const int32_t kt0 = kt_ids[0], kt1 = (kt_n > 1 ? kt_ids[1] : -1);
        const int K_N = k_n;

        std::vector<std::thread> workers;
        workers.reserve(nthreads);

        size_t chunk = (title_rows + nthreads - 1) / nthreads;
        for (int t = 0; t < nthreads; ++t) {
            size_t lo = (size_t)t * chunk;
            size_t hi = std::min(lo + chunk, title_rows);
            workers.emplace_back([&, t, lo, hi]() {
                StrBuf& min_rating = th_min_rating[t];
                StrBuf& min_title  = th_min_title[t];
                int32_t local_k_ids[8];
                for (int j = 0; j < K_N; ++j) local_k_ids[j] = k_ids[j];

                for (size_t i = lo; i < hi; ++i) {
                    // Year filter (NULL = INT32_MIN handles via > 2005 check)
                    int32_t yr = py[i];
                    if (yr <= 2005) continue;
                    int32_t kid = kid_col[i];
                    if (kid != kt0 && kid != kt1) continue;

                    int32_t mid = (int32_t)(i + 1);

                    // (b) keyword probe — short-circuit
                    int32_t mklo = mk_off[mid];
                    int32_t mkhi = mk_off[mid + 1];
                    bool kw_ok = false;
                    for (int32_t r = mklo; r < mkhi && !kw_ok; ++r) {
                        int32_t v = mk_kw[r];
                        for (int j = 0; j < K_N; ++j) {
                            if (v == local_k_ids[j]) { kw_ok = true; break; }
                        }
                    }
                    if (!kw_ok) continue;

                    // (c) country probe — short-circuit
                    int32_t milo = mi_off[mid];
                    int32_t mihi = mi_off[mid + 1];
                    bool country_ok = false;
                    for (int32_t r = milo; r < mihi && !country_ok; ++r) {
                        if (mi_itid[r] != it1_id) continue;
                        int64_t s = mi_io[r], e = mi_io[r + 1];
                        size_t l = (size_t)(e - s);
                        if (is_country(mi_id + s, l)) country_ok = true;
                    }
                    if (!country_ok) continue;

                    // (d) rating scan + aggregate
                    int32_t miilo = mii_off[mid];
                    int32_t miihi = mii_off[mid + 1];
                    bool any_rating = false;
                    for (int32_t r = miilo; r < miihi; ++r) {
                        if (mii_itid[r] != it2_id) continue;
                        int64_t s = mii_io[r], e = mii_io[r + 1];
                        size_t l = (size_t)(e - s);
                        const char* p = mii_id + s;
                        if (lex_lt_85(p, l)) {
                            min_rating.try_min(p, l);
                            any_rating = true;
                        }
                    }
                    if (!any_rating) continue;

                    int64_t ts = t_to[i], te = t_to[i + 1];
                    min_title.try_min(t_td + ts, (size_t)(te - ts));
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    StrBuf min_rating, min_title;
    for (int t = 0; t < nthreads; ++t) {
        min_rating.try_min(th_min_rating[t]);
        min_title.try_min(th_min_title[t]);
    }

    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q14c.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "rating,north_european_dark_production\n");
        if (min_rating.has) std::fwrite(min_rating.buf.data(), 1, min_rating.buf.size(), f);
        std::fputc(',', f);
        if (min_title.has) std::fwrite(min_title.buf.data(), 1, min_title.buf.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
