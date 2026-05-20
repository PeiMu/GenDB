// Q33b: IMDB-JOB
// Strategy: drive via movie_link CSR on link_type_id for follow-like lt_ids.
// Six MIN aggregations.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static inline int lex_cmp(const char* a, size_t la, const char* b, size_t lb) {
    size_t n = la < lb ? la : lb;
    int c = std::memcmp(a, b, n);
    if (c) return c;
    if (la == lb) return 0;
    return la < lb ? -1 : 1;
}

// memmem polyfill in case missing
static inline const void* my_memmem(const void* hay, size_t hlen,
                                    const void* needle, size_t nlen) {
    return memmem(hay, hlen, needle, nlen);
}

struct VarLen {
    const uint64_t* off;
    const char* dat;
    size_t off_count;
    inline const char* ptr(int32_t i, size_t& len) const {
        uint64_t s = off[i];
        uint64_t e = off[i+1];
        len = (size_t)(e - s);
        return dat + s;
    }
};

static void load_varlen(const std::string& base, MmapColumn<uint64_t>& off, MmapColumn<char>& dat, VarLen& v) {
    off.open(base + ".off");
    dat.open(base + ".dat");
    v.off = off.data;
    v.dat = dat.data;
    v.off_count = off.count;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];
    mkdir(results.c_str(), 0755);

    // ===== Load all columns / indexes (mmap) =====
    // dim columns
    MmapColumn<int16_t> cn_cc;
    MmapColumn<uint64_t> cn_cc_off; MmapColumn<char> cn_cc_dat;
    MmapColumn<uint64_t> cn_name_off; MmapColumn<char> cn_name_dat;

    MmapColumn<uint64_t> it_info_off; MmapColumn<char> it_info_dat;
    MmapColumn<uint64_t> kt_kind_off; MmapColumn<char> kt_kind_dat;
    MmapColumn<uint64_t> lt_link_off; MmapColumn<char> lt_link_dat;

    // title
    MmapColumn<int32_t> t_kind_id;
    MmapColumn<int32_t> t_year;
    MmapColumn<uint64_t> t_title_off; MmapColumn<char> t_title_dat;

    // movie_link
    MmapColumn<int32_t> ml_movie_id;
    MmapColumn<int32_t> ml_linked;
    MmapColumn<int32_t> ml_lt_id;

    // movie_companies
    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_id;

    // movie_info_idx
    MmapColumn<int32_t> mi_movie_id;
    MmapColumn<int32_t> mi_it_id;
    MmapColumn<uint64_t> mi_info_off; MmapColumn<char> mi_info_dat;

    // indexes
    MmapColumn<int32_t> mll_off, mll_rid;
    MmapColumn<int32_t> mc_mid_off;
    MmapColumn<int32_t> mi_mid_off;

    VarLen cn_cc_d{}, cn_name{}, it_info{}, kt_kind{}, lt_link{}, t_title{}, mi_info{};

    {
        GENDB_PHASE("data_loading");
        // company_name
        cn_cc.open(gendb + "/company_name/country_code.bin");
        load_varlen(gendb + "/company_name/country_code.dict", cn_cc_off, cn_cc_dat, cn_cc_d);
        load_varlen(gendb + "/company_name/name", cn_name_off, cn_name_dat, cn_name);

        // info_type, kind_type, link_type
        load_varlen(gendb + "/info_type/info", it_info_off, it_info_dat, it_info);
        load_varlen(gendb + "/kind_type/kind", kt_kind_off, kt_kind_dat, kt_kind);
        load_varlen(gendb + "/link_type/link", lt_link_off, lt_link_dat, lt_link);

        // title
        t_kind_id.open(gendb + "/title/kind_id.bin");
        t_year.open(gendb + "/title/production_year.bin");
        load_varlen(gendb + "/title/title", t_title_off, t_title_dat, t_title);

        // movie_link
        ml_movie_id.open(gendb + "/movie_link/movie_id.bin");
        ml_linked.open(gendb + "/movie_link/linked_movie_id.bin");
        ml_lt_id.open(gendb + "/movie_link/link_type_id.bin");

        // movie_companies
        mc_movie_id.open(gendb + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb + "/movie_companies/company_id.bin");

        // movie_info_idx
        mi_movie_id.open(gendb + "/movie_info_idx/movie_id.bin");
        mi_it_id.open(gendb + "/movie_info_idx/info_type_id.bin");
        load_varlen(gendb + "/movie_info_idx/info", mi_info_off, mi_info_dat, mi_info);

        // indexes
        mll_off.open(gendb + "/_idx/movie_link__link_type_id__offsets.bin");
        mll_rid.open(gendb + "/_idx/movie_link__link_type_id__rowids.bin");
        mc_mid_off.open(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_mid_off.open(gendb + "/_idx/movie_info_idx__movie_id__offsets.bin");

        // hint random access for probe columns
        mc_movie_id.advise_random();
        mc_company_id.advise_random();
        mi_movie_id.advise_random();
        mi_it_id.advise_random();
        t_kind_id.advise_random();
        t_year.advise_random();
    }

    // ===== Resolve dim values =====
    // company_name nl_code
    int16_t nl_code = -1;
    {
        // varlen dict: cn_cc_d has off_count = #codes+1
        size_t ndict = cn_cc_d.off_count - 1;
        for (size_t i = 0; i < ndict; i++) {
            size_t L; const char* p = cn_cc_d.ptr((int32_t)i, L);
            // Dict codes are 1-based (0 reserved for NULL)
            if (L == 4 && std::memcmp(p, "[nl]", 4) == 0) { nl_code = (int16_t)(i + 1); break; }
        }
    }
    if (nl_code < 0) { std::fprintf(stderr, "[nl] code not found\n"); return 2; }

    // Build cn1_ids bitmap (cn1_ids[i] true if cn.country_code[i-1]==nl_code, id is i+1 since ids are 1-based but row index i maps to id i+1? Actually title id is row+1; for company_name same convention)
    // Use a vector<bool>-style packed bitmap indexed by company_id (1-based), size = cn rows + 2.
    size_t ncn = cn_cc.count;
    std::vector<uint64_t> cn_nl_bits((ncn + 64) / 64 + 1, 0);
    for (size_t i = 0; i < ncn; i++) {
        if (cn_cc[i] == nl_code) {
            // company_id = i+1
            size_t id = i + 1;
            cn_nl_bits[id >> 6] |= (uint64_t)1 << (id & 63);
        }
    }
    auto is_nl = [&](int32_t cid) -> bool {
        if (cid <= 0) return false;
        size_t u = (size_t)cid;
        if ((u >> 6) >= cn_nl_bits.size()) return false;
        return (cn_nl_bits[u >> 6] >> (u & 63)) & 1ULL;
    };

    // rating_id (info_type)
    int32_t rating_id = -1;
    {
        size_t n = it_info.off_count - 1;
        for (size_t i = 0; i < n; i++) {
            size_t L; const char* p = it_info.ptr((int32_t)i, L);
            if (L == 6 && std::memcmp(p, "rating", 6) == 0) { rating_id = (int32_t)(i + 1); break; }
        }
    }
    if (rating_id < 0) { std::fprintf(stderr, "rating info_type not found\n"); return 2; }

    // tv_kind_id (kind_type)
    int32_t tv_kind_id = -1;
    {
        size_t n = kt_kind.off_count - 1;
        for (size_t i = 0; i < n; i++) {
            size_t L; const char* p = kt_kind.ptr((int32_t)i, L);
            if (L == 9 && std::memcmp(p, "tv series", 9) == 0) { tv_kind_id = (int32_t)(i + 1); break; }
        }
    }
    if (tv_kind_id < 0) { std::fprintf(stderr, "tv series kind not found\n"); return 2; }

    // lt_ids (link_type LIKE '%follow%')
    std::vector<int32_t> lt_ids;
    {
        size_t n = lt_link.off_count - 1;
        for (size_t i = 0; i < n; i++) {
            size_t L; const char* p = lt_link.ptr((int32_t)i, L);
            if (L >= 6 && my_memmem(p, L, "follow", 6) != nullptr) {
                lt_ids.push_back((int32_t)(i + 1));
            }
        }
    }
    if (lt_ids.empty()) { std::fprintf(stderr, "no follow link types\n"); return 2; }

    // ===== Main scan =====
    // Six running MINs: keep ptr+len
    const char* min_cn1 = nullptr; size_t min_cn1_len = 0;
    const char* min_cn2 = nullptr; size_t min_cn2_len = 0;
    const char* min_r1 = nullptr;  size_t min_r1_len = 0;
    const char* min_r2 = nullptr;  size_t min_r2_len = 0;
    const char* min_t1 = nullptr;  size_t min_t1_len = 0;
    const char* min_t2 = nullptr;  size_t min_t2_len = 0;

    auto upd_min = [](const char*& cur, size_t& cur_len, const char* p, size_t L) {
        if (cur == nullptr || lex_cmp(p, L, cur, cur_len) < 0) {
            cur = p; cur_len = L;
        }
    };

    size_t n_title_kind = t_kind_id.count;
    size_t n_title_year = t_year.count;
    (void)n_title_year;

    {
        GENDB_PHASE("main_scan");

        // mll_off has 20 entries; lt_id 1..18 plus sentinel
        size_t lt_off_count = mll_off.count;

        for (int32_t lt_id : lt_ids) {
            if ((size_t)(lt_id + 1) >= lt_off_count) continue;
            int32_t lo = mll_off[lt_id];
            int32_t hi = mll_off[lt_id + 1];
            for (int32_t k = lo; k < hi; k++) {
                int32_t r = mll_rid[k];
                int32_t t1_id = ml_movie_id[r];
                int32_t t2_id = ml_linked[r];

                // bounds (defensive)
                if (t1_id <= 0 || t2_id <= 0) continue;
                if ((size_t)(t1_id - 1) >= n_title_kind) continue;
                if ((size_t)(t2_id - 1) >= n_title_kind) continue;

                // Apply most selective: t2 year == 2007
                if (t_year[t2_id - 1] != 2007) continue;
                // t2 kind == tv
                if (t_kind_id[t2_id - 1] != tv_kind_id) continue;
                // t1 kind == tv
                if (t_kind_id[t1_id - 1] != tv_kind_id) continue;

                // mc1: walk movie_companies for t1_id, find nl company
                int32_t mc_lo1 = mc_mid_off[t1_id];
                int32_t mc_hi1 = mc_mid_off[t1_id + 1];
                const char* this_cn1 = nullptr; size_t this_cn1_len = 0;
                for (int32_t i = mc_lo1; i < mc_hi1; i++) {
                    int32_t cid = mc_company_id[i];
                    if (is_nl(cid)) {
                        size_t L; const char* p = cn_name.ptr(cid - 1, L);
                        // Find MIN over qualifying
                        if (this_cn1 == nullptr || lex_cmp(p, L, this_cn1, this_cn1_len) < 0) {
                            this_cn1 = p; this_cn1_len = L;
                        }
                    }
                }
                if (this_cn1 == nullptr) continue;

                // mc2: walk for t2_id, any company; for MIN we want smallest cn2 name overall
                int32_t mc_lo2 = mc_mid_off[t2_id];
                int32_t mc_hi2 = mc_mid_off[t2_id + 1];
                if (mc_lo2 >= mc_hi2) continue;
                const char* this_cn2 = nullptr; size_t this_cn2_len = 0;
                for (int32_t i = mc_lo2; i < mc_hi2; i++) {
                    int32_t cid = mc_company_id[i];
                    if (cid <= 0) continue;
                    size_t L; const char* p = cn_name.ptr(cid - 1, L);
                    if (this_cn2 == nullptr || lex_cmp(p, L, this_cn2, this_cn2_len) < 0) {
                        this_cn2 = p; this_cn2_len = L;
                    }
                }
                if (this_cn2 == nullptr) continue;

                // mi_idx1: walk for t1_id, info_type==rating; capture for MIN
                int32_t mi_lo1 = mi_mid_off[t1_id];
                int32_t mi_hi1 = mi_mid_off[t1_id + 1];
                const char* this_r1 = nullptr; size_t this_r1_len = 0;
                for (int32_t i = mi_lo1; i < mi_hi1; i++) {
                    if (mi_it_id[i] != rating_id) continue;
                    size_t L; const char* p = mi_info.ptr(i, L);
                    if (this_r1 == nullptr || lex_cmp(p, L, this_r1, this_r1_len) < 0) {
                        this_r1 = p; this_r1_len = L;
                    }
                }
                if (this_r1 == nullptr) continue;

                // mi_idx2: walk for t2_id, info_type==rating, info<'3.0' lex
                int32_t mi_lo2 = mi_mid_off[t2_id];
                int32_t mi_hi2 = mi_mid_off[t2_id + 1];
                const char* this_r2 = nullptr; size_t this_r2_len = 0;
                for (int32_t i = mi_lo2; i < mi_hi2; i++) {
                    if (mi_it_id[i] != rating_id) continue;
                    size_t L; const char* p = mi_info.ptr(i, L);
                    // lex < "3.0"
                    if (lex_cmp(p, L, "3.0", 3) >= 0) continue;
                    if (this_r2 == nullptr || lex_cmp(p, L, this_r2, this_r2_len) < 0) {
                        this_r2 = p; this_r2_len = L;
                    }
                }
                if (this_r2 == nullptr) continue;

                // titles
                size_t L1; const char* p_t1 = t_title.ptr(t1_id - 1, L1);
                size_t L2; const char* p_t2 = t_title.ptr(t2_id - 1, L2);

                // update global MINs
                upd_min(min_cn1, min_cn1_len, this_cn1, this_cn1_len);
                upd_min(min_cn2, min_cn2_len, this_cn2, this_cn2_len);
                upd_min(min_r1, min_r1_len, this_r1, this_r1_len);
                upd_min(min_r2, min_r2_len, this_r2, this_r2_len);
                upd_min(min_t1, min_t1_len, p_t1, L1);
                upd_min(min_t2, min_t2_len, p_t2, L2);
            }
        }
    }

    // ===== Output =====
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q33b.csv";
        FILE* f = std::fopen(outpath.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 3; }
        std::fputs("first_company,second_company,first_rating,second_rating,first_movie,second_movie\n", f);
        if (min_cn1 == nullptr) {
            // No rows
            std::fputs(",,,,,\n", f);
        } else {
            auto write_csv = [&](const char* p, size_t L, bool last) {
                // Check if needs quoting (contains comma, quote, newline)
                bool need = false;
                for (size_t i = 0; i < L; i++) {
                    char c = p[i];
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
                }
                if (need) {
                    std::fputc('"', f);
                    for (size_t i = 0; i < L; i++) {
                        char c = p[i];
                        if (c == '"') std::fputc('"', f);
                        std::fputc(c, f);
                    }
                    std::fputc('"', f);
                } else {
                    std::fwrite(p, 1, L, f);
                }
                std::fputc(last ? '\n' : ',', f);
            };
            write_csv(min_cn1, min_cn1_len, false);
            write_csv(min_cn2, min_cn2_len, false);
            write_csv(min_r1, min_r1_len, false);
            write_csv(min_r2, min_r2_len, false);
            write_csv(min_t1, min_t1_len, false);
            write_csv(min_t2, min_t2_len, true);
        }
        std::fclose(f);
    }

    return 0;
}
