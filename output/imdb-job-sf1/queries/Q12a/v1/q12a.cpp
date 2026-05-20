// Q12a - IMDB JOB
// SELECT MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// FROM company_name cn, company_type ct, info_type it1, info_type it2,
//      movie_companies mc, movie_info mi, movie_info_idx mi_idx, title t
// WHERE cn.country_code = '[us]' AND ct.kind = 'production companies'
//   AND it1.info = 'genres' AND it2.info = 'rating'
//   AND mi.info IN ('Drama','Horror') AND mi_idx.info > '8.0'
//   AND t.production_year BETWEEN 2005 AND 2008
//   AND joins on movie_id, info_type_id, company_type_id, company_id

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// Returns true if (a,la) > (b,lb) lexicographically (byte-wise).
static inline bool lex_gt(const char* a, size_t la, const char* b, size_t lb) {
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c > 0;
    return la > lb;
}

// Returns negative/zero/positive: lex cmp (a,la) vs (b,lb).
static inline int lex_cmp(const char* a, size_t la, const char* b, size_t lb) {
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

struct StrBuf {
    std::vector<char> buf;
    bool has = false;
    void set(const char* p, size_t n) {
        buf.assign(p, p + n);
        has = true;
    }
    bool empty() const { return !has; }
    void try_min(const char* p, size_t n) {
        if (!has) { set(p, n); return; }
        if (lex_cmp(p, n, buf.data(), buf.size()) < 0) set(p, n);
    }
    void try_min(const StrBuf& other) {
        if (!other.has) return;
        if (!has) { buf = other.buf; has = true; return; }
        if (lex_cmp(other.buf.data(), other.buf.size(), buf.data(), buf.size()) < 0) buf = other.buf;
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

    // --- Load (mmap) all columns/indexes ---
    MmapColumn<int64_t> title_title_off, ct_kind_off, it_info_off, cc_dict_off, cn_name_off;
    MmapColumn<int64_t> mii_info_off, mc_kind_off_unused;
    MmapColumn<char>    title_title_dat, ct_kind_dat, it_info_dat, cc_dict_dat, cn_name_dat;
    MmapColumn<char>    mii_info_dat;
    MmapColumn<int32_t> title_id, title_py;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int32_t> mii_movie_id, mii_info_type_id;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int32_t> mc_movie_id, mc_company_id, mc_company_type_id;
    MmapColumn<int32_t> mii_off_idx, mi_off_idx, mc_off_idx;

    {
        GENDB_PHASE("data_loading");
        title_title_off.open(gendb_dir + "/title/title.off");
        title_title_dat.open(gendb_dir + "/title/title.dat");
        title_id.open(gendb_dir + "/title/id.bin");
        title_py.open(gendb_dir + "/title/production_year.bin");

        ct_kind_off.open(gendb_dir + "/company_type/kind.off");
        ct_kind_dat.open(gendb_dir + "/company_type/kind.dat");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");
        cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        mii_movie_id.open(gendb_dir + "/movie_info_idx/movie_id.bin");
        mii_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mii_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        mii_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        mi_movie_id.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");

        mii_off_idx.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
    }

    // --- Resolve dim literals ---
    int32_t ct_id = -1;
    int32_t it1_id = -1; // genres
    int32_t it2_id = -1; // rating
    int16_t us_code = -1;

    {
        GENDB_PHASE("resolve_dims");
        // company_type kind = 'production companies'
        {
            const char* target = "production companies";
            size_t tlen = std::strlen(target);
            size_t n = ct_kind_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = ct_kind_off.data[i], e = ct_kind_off.data[i+1];
                size_t l = (size_t)(e - s);
                if (l == tlen && std::memcmp(ct_kind_dat.data + s, target, tlen) == 0) {
                    ct_id = (int32_t)(i + 1); // 1-indexed id
                    break;
                }
            }
        }
        // info_type info='genres' and 'rating'
        {
            const char* g = "genres"; size_t gl = 6;
            const char* r = "rating"; size_t rl = 6;
            size_t n = it_info_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = it_info_off.data[i], e = it_info_off.data[i+1];
                size_t l = (size_t)(e - s);
                if (l == gl && std::memcmp(it_info_dat.data + s, g, gl) == 0) {
                    it1_id = (int32_t)(i + 1);
                }
                if (l == rl && std::memcmp(it_info_dat.data + s, r, rl) == 0) {
                    it2_id = (int32_t)(i + 1);
                }
            }
        }
        // country_code dict: find '[us]'
        // Dict encoding: code 0 = NULL; real codes start at 1, so stored value = dict_entry_index + 1.
        {
            const char* us = "[us]"; size_t ul = 4;
            size_t n = cc_dict_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = cc_dict_off.data[i], e = cc_dict_off.data[i+1];
                size_t l = (size_t)(e - s);
                if (l == ul && std::memcmp(cc_dict_dat.data + s, us, ul) == 0) {
                    us_code = (int16_t)(i + 1);
                    break;
                }
            }
        }
    }

    if (ct_id < 0 || it1_id < 0 || it2_id < 0 || us_code < 0) {
        std::fprintf(stderr, "Dim resolution failed: ct=%d it1=%d it2=%d us=%d\n",
                     ct_id, it1_id, it2_id, (int)us_code);
        return 2;
    }

    // --- Parallel scan ---
    size_t title_rows = title_py.count;
    int nthreads = (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 12) nthreads = 12;

    std::vector<StrBuf> th_min_cn(nthreads), th_min_rating(nthreads), th_min_title(nthreads);

    {
        GENDB_PHASE("main_scan");

        const int32_t* py = title_py.data;
        const int32_t* mii_off = mii_off_idx.data;
        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* mc_off = mc_off_idx.data;
        const int32_t* mii_itid = mii_info_type_id.data;
        const int64_t* mii_io = mii_info_off.data;
        const char*    mii_id = mii_info_dat.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int64_t* mi_io = mi_info_off.data;
        const char*    mi_id = mi_info_dat.data;
        const int32_t* mc_ctid = mc_company_type_id.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int16_t* cn_cc = cn_country_code.data;
        const int64_t* cn_no = cn_name_off.data;
        const char*    cn_nd = cn_name_dat.data;
        const int64_t* t_to = title_title_off.data;
        const char*    t_td = title_title_dat.data;

        std::vector<std::thread> workers;
        workers.reserve(nthreads);

        size_t chunk = (title_rows + nthreads - 1) / nthreads;
        for (int t = 0; t < nthreads; ++t) {
            size_t lo = (size_t)t * chunk;
            size_t hi = std::min(lo + chunk, title_rows);
            workers.emplace_back([&, t, lo, hi]() {
                StrBuf &min_cn = th_min_cn[t];
                StrBuf &min_rating = th_min_rating[t];
                StrBuf &min_title = th_min_title[t];

                for (size_t i = lo; i < hi; ++i) {
                    int32_t yr = py[i];
                    if (yr < 2005 || yr > 2008) continue;
                    int32_t mid = (int32_t)(i + 1); // dense PK, 1-indexed

                    // -- Check movie_info_idx: any row with info_type_id==it2_id AND info > "8.0"
                    int32_t miilo = mii_off[mid];
                    int32_t miihi = mii_off[mid + 1];
                    // gather qualifying rating strings (their min)
                    StrBuf local_rating;
                    for (int32_t r = miilo; r < miihi; ++r) {
                        if (mii_itid[r] != it2_id) continue;
                        int64_t s = mii_io[r], e = mii_io[r + 1];
                        size_t l = (size_t)(e - s);
                        if (lex_gt(mii_id + s, l, "8.0", 3)) {
                            local_rating.try_min(mii_id + s, l);
                        }
                    }
                    if (local_rating.empty()) continue;

                    // -- Check movie_info: existence of row with info_type_id==it1_id AND info IN ('Drama','Horror')
                    int32_t milo = mi_off[mid];
                    int32_t mihi = mi_off[mid + 1];
                    bool genre_ok = false;
                    for (int32_t r = milo; r < mihi && !genre_ok; ++r) {
                        if (mi_itid[r] != it1_id) continue;
                        int64_t s = mi_io[r], e = mi_io[r + 1];
                        size_t l = (size_t)(e - s);
                        const char* p = mi_id + s;
                        if (l == 5) {
                            if (std::memcmp(p, "Drama", 5) == 0) genre_ok = true;
                        } else if (l == 6) {
                            if (std::memcmp(p, "Horror", 6) == 0) genre_ok = true;
                        }
                    }
                    if (!genre_ok) continue;

                    // -- Check movie_companies: row with company_type_id==ct_id AND cn.country_code[cid-1]==us_code
                    int32_t mclo = mc_off[mid];
                    int32_t mchi = mc_off[mid + 1];
                    StrBuf local_cn;
                    for (int32_t r = mclo; r < mchi; ++r) {
                        if (mc_ctid[r] != ct_id) continue;
                        int32_t cid = mc_cid[r];
                        if (cid < 1) continue;
                        if (cn_cc[cid - 1] != us_code) continue;
                        int64_t ns = cn_no[cid - 1], ne = cn_no[cid];
                        local_cn.try_min(cn_nd + ns, (size_t)(ne - ns));
                    }
                    if (local_cn.empty()) continue;

                    // -- Qualifies: update three MINs
                    min_cn.try_min(local_cn);
                    min_rating.try_min(local_rating);
                    int64_t ts = t_to[i], te = t_to[i + 1];
                    min_title.try_min(t_td + ts, (size_t)(te - ts));
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Merge thread-local mins
    StrBuf min_cn, min_rating, min_title;
    for (int t = 0; t < nthreads; ++t) {
        min_cn.try_min(th_min_cn[t]);
        min_rating.try_min(th_min_rating[t]);
        min_title.try_min(th_min_title[t]);
    }

    // --- Output CSV ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q12a.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "movie_company,rating,drama_horror_movie\n");
        auto emit = [&](const StrBuf& s) {
            if (!s.has) { std::fprintf(f, ""); return; }
            std::fwrite(s.buf.data(), 1, s.buf.size(), f);
        };
        emit(min_cn);
        std::fputc(',', f);
        emit(min_rating);
        std::fputc(',', f);
        emit(min_title);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
