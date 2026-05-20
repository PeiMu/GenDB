// Q12b - IMDB JOB
// SELECT MIN(mi.info) AS budget, MIN(t.title) AS unsuccsessful_movie
// Filters: cn.country_code = '[us]', ct.kind IN ('production companies','distributors'),
//          it1.info='budget', it2.info='bottom 10 rank',
//          t.production_year > 2000, t.title LIKE 'Birdemic%' OR t.title LIKE '%Movie%'
// Drive: movie_info_idx CSR on info_type_id == it2_id (very rare).

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

struct StrBuf {
    std::vector<char> buf;
    bool has = false;
    void set(const char* p, size_t n) {
        buf.assign(p, p + n);
        has = true;
    }
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

// Write a CSV field with proper quoting (RFC 4180).
static void write_csv_field(FILE* f, const char* p, size_t n) {
    bool need_quote = false;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        std::fwrite(p, 1, n, f);
    } else {
        std::fputc('"', f);
        for (size_t i = 0; i < n; ++i) {
            char c = p[i];
            if (c == '"') std::fputc('"', f);
            std::fputc(c, f);
        }
        std::fputc('"', f);
    }
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

    // --- Load (mmap) all columns/indexes ---
    MmapColumn<int64_t> title_title_off, ct_kind_off, it_info_off, cc_dict_off;
    MmapColumn<char>    title_title_dat, ct_kind_dat, it_info_dat, cc_dict_dat;
    MmapColumn<int32_t> title_py;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int32_t> mii_movie_id, mii_info_type_id;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int32_t> mc_movie_id, mc_company_id, mc_company_type_id;
    MmapColumn<int32_t> mi_off_idx, mc_off_idx;

    // CSR for movie_info_idx info_type_id
    MmapColumn<int32_t> mii_it_off, mii_it_rowids;

    {
        GENDB_PHASE("data_loading");
        title_title_off.open(gendb_dir + "/title/title.off");
        title_title_dat.open(gendb_dir + "/title/title.dat");
        title_py.open(gendb_dir + "/title/production_year.bin");

        ct_kind_off.open(gendb_dir + "/company_type/kind.off");
        ct_kind_dat.open(gendb_dir + "/company_type/kind.dat");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");
        cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");

        mii_movie_id.open(gendb_dir + "/movie_info_idx/movie_id.bin");
        mii_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");

        mi_movie_id.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");

        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");

        mii_it_off.open(gendb_dir + "/_idx/movie_info_idx__info_type_id__offsets.bin");
        mii_it_rowids.open(gendb_dir + "/_idx/movie_info_idx__info_type_id__rowids.bin");
    }

    // --- Resolve dim literals ---
    int32_t ct_ids[2] = { -1, -1 };
    int ct_n = 0;
    int32_t it1_id = -1; // 'budget'
    int32_t it2_id = -1; // 'bottom 10 rank'
    int16_t us_code = -1;

    {
        GENDB_PHASE("resolve_dims");
        // company_type kind set
        {
            const char* targets[2] = { "production companies", "distributors" };
            size_t tlens[2] = { 20, 12 };
            size_t n = ct_kind_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = ct_kind_off.data[i], e = ct_kind_off.data[i+1];
                size_t l = (size_t)(e - s);
                for (int k = 0; k < 2; ++k) {
                    if (l == tlens[k] && std::memcmp(ct_kind_dat.data + s, targets[k], tlens[k]) == 0) {
                        ct_ids[ct_n++] = (int32_t)(i + 1);
                    }
                }
            }
        }
        // info_type
        {
            const char* a = "budget"; size_t al = 6;
            const char* b = "bottom 10 rank"; size_t bl = 14;
            size_t n = it_info_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = it_info_off.data[i], e = it_info_off.data[i+1];
                size_t l = (size_t)(e - s);
                if (l == al && std::memcmp(it_info_dat.data + s, a, al) == 0) {
                    it1_id = (int32_t)(i + 1);
                }
                if (l == bl && std::memcmp(it_info_dat.data + s, b, bl) == 0) {
                    it2_id = (int32_t)(i + 1);
                }
            }
        }
        // country_code dict
        {
            const char* us = "[us]"; size_t ul = 4;
            size_t n = cc_dict_off.count - 1;
            for (size_t i = 0; i < n; ++i) {
                int64_t s = cc_dict_off.data[i], e = cc_dict_off.data[i+1];
                size_t l = (size_t)(e - s);
                if (l == ul && std::memcmp(cc_dict_dat.data + s, us, ul) == 0) {
                    // Binary country_code values are dict_index + 1 (0 is reserved for NULL).
                    us_code = (int16_t)(i + 1);
                    break;
                }
            }
        }
    }

    if (ct_n == 0 || it1_id < 0 || it2_id < 0 || us_code < 0) {
        std::fprintf(stderr, "Dim resolution failed: ct_n=%d it1=%d it2=%d us=%d\n",
                     ct_n, it1_id, it2_id, (int)us_code);
        return 2;
    }

    int32_t ct0 = ct_ids[0];
    int32_t ct1 = (ct_n >= 2) ? ct_ids[1] : ct_ids[0]; // duplicate if only one

    // --- Drive the mi_idx CSR for it2_id ('bottom 10 rank') ---
    // Range [lo, hi) of mi_idx row ids matching info_type_id == it2_id.
    int32_t lo = mii_it_off.data[it2_id];
    int32_t hi = mii_it_off.data[it2_id + 1];

    int nthreads = (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 12) nthreads = 12;
    // For small CSR ranges, don't oversubscribe.
    int n_candidates = hi - lo;
    if (n_candidates < nthreads) nthreads = std::max(1, n_candidates);

    std::vector<StrBuf> th_min_budget(nthreads), th_min_title(nthreads);

    {
        GENDB_PHASE("main_scan");

        const int32_t* py = title_py.data;
        const int32_t* mii_mid = mii_movie_id.data;
        const int32_t* mii_rowids = mii_it_rowids.data;
        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* mc_off = mc_off_idx.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int64_t* mi_io = mi_info_off.data;
        const char*    mi_id = mi_info_dat.data;
        const int32_t* mc_ctid = mc_company_type_id.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int16_t* cn_cc = cn_country_code.data;
        const int64_t* t_to = title_title_off.data;
        const char*    t_td = title_title_dat.data;

        std::vector<std::thread> workers;
        workers.reserve(nthreads);

        int chunk = (n_candidates + nthreads - 1) / nthreads;
        for (int t = 0; t < nthreads; ++t) {
            int wlo = lo + t * chunk;
            int whi = std::min(lo + (t + 1) * chunk, hi);
            if (wlo >= whi) continue;
            workers.emplace_back([&, t, wlo, whi]() {
                StrBuf &min_budget = th_min_budget[t];
                StrBuf &min_title = th_min_title[t];

                for (int k = wlo; k < whi; ++k) {
                    int32_t mii_row = mii_rowids[k];
                    int32_t mid = mii_mid[mii_row];
                    if (mid < 1) continue;
                    int32_t i = mid - 1; // title row index (dense PK)

                    // Title filters
                    int32_t yr = py[i];
                    if (yr == INT32_MIN || yr <= 2000) continue;

                    int64_t ts = t_to[i], te = t_to[i + 1];
                    size_t tl = (size_t)(te - ts);
                    const char* tp = t_td + ts;

                    // 'Birdemic%' prefix (len>=8, memcmp prefix)
                    bool title_ok = false;
                    if (tl >= 8 && std::memcmp(tp, "Birdemic", 8) == 0) {
                        title_ok = true;
                    } else if (tl >= 5) {
                        // '%Movie%' substring search
                        if (memmem(tp, tl, "Movie", 5) != nullptr) title_ok = true;
                    }
                    if (!title_ok) continue;

                    // Probe movie_info for any row with info_type_id == it1_id; capture all qualifying mi.info into local MIN
                    int32_t milo = mi_off[mid];
                    int32_t mihi = mi_off[mid + 1];
                    StrBuf local_budget;
                    for (int32_t r = milo; r < mihi; ++r) {
                        if (mi_itid[r] != it1_id) continue;
                        int64_t s = mi_io[r], e = mi_io[r + 1];
                        size_t l = (size_t)(e - s);
                        local_budget.try_min(mi_id + s, l);
                    }
                    if (!local_budget.has) continue;

                    // Probe movie_companies for any row with company_type_id IN CT AND cn.country_code[cid-1] == us_code
                    int32_t mclo = mc_off[mid];
                    int32_t mchi = mc_off[mid + 1];
                    bool mc_ok = false;
                    for (int32_t r = mclo; r < mchi; ++r) {
                        int32_t ctid = mc_ctid[r];
                        if (ctid != ct0 && ctid != ct1) continue;
                        int32_t cid = mc_cid[r];
                        if (cid < 1) continue;
                        if (cn_cc[cid - 1] != us_code) continue;
                        mc_ok = true;
                        break;
                    }
                    if (!mc_ok) continue;

                    // Inner join succeeds: fold into thread-local MINs
                    min_budget.try_min(local_budget);
                    min_title.try_min(tp, tl);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Merge thread-local mins
    StrBuf min_budget, min_title;
    for (int t = 0; t < nthreads; ++t) {
        min_budget.try_min(th_min_budget[t]);
        min_title.try_min(th_min_title[t]);
    }

    // --- Output CSV ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q12b.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "budget,unsuccsessful_movie\n");
        if (min_budget.has) write_csv_field(f, min_budget.buf.data(), min_budget.buf.size());
        std::fputc(',', f);
        if (min_title.has) write_csv_field(f, min_title.buf.data(), min_title.buf.size());
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
