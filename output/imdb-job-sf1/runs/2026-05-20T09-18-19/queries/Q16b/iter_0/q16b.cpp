// Q16b: MIN(an.name), MIN(t.title) over join {aka_name, cast_info, company_name,
// keyword, movie_companies, movie_keyword, name, title} with
//   cn.country_code='[us]' AND k.keyword='character-name-in-title'
// Strategy (per plan.json):
//   1. Resolve us_code (dict) and k_id (varlen scan).
//   2. Build per-company US mask (uint8[]).
//   3. CSR enumerate movie_keyword for k_id → unique movie_id bitset.
//   4. Filter movies to those with >=1 US movie_companies row → qualifying_movies[].
//   5. Parallel over qualifying_movies: for each mv, iterate ci rows, then for each
//      pid iterate aka_name rows; track local MIN(an.name) and MIN(t.title).
//      Reduce at end.
//   6. CSV output.

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <filesystem>

using namespace gendb;

// Linear scan for a target string in a varlen column (.off int64, .dat bytes).
// Returns the row index (0-based) of the first match, or -1 if not found.
static int64_t find_varlen_row(const int64_t* off, size_t n, const char* dat,
                               const std::string& target) {
    size_t tlen = target.size();
    for (size_t i = 0; i < n; ++i) {
        size_t lo = (size_t)off[i];
        size_t hi = (size_t)off[i + 1];
        if (hi - lo == tlen && std::memcmp(dat + lo, target.data(), tlen) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

// CSV-quote a field if it contains comma, double-quote, or newline.
static std::string csv_quote(const std::string& s) {
    bool need = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
    }
    if (!need) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: q16b <gendb_dir> <results_dir>\n");
        return 1;
    }
    std::string store   = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    // Defaults from template SQL
    std::string p_country = parse_string_arg(argc, argv, "--country_code", "[us]");
    std::string p_keyword = parse_string_arg(argc, argv, "--keyword", "character-name-in-title");

    // --------------- Open all needed columns / indexes (mmap, zero-copy) ----------------
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cc_dict_off;
    MmapColumn<char>    cc_dict_dat;

    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;

    MmapColumn<int32_t> mk_k_off;
    MmapColumn<int32_t> mk_k_row;
    MmapColumn<int32_t> mk_movie;

    MmapColumn<int32_t> mc_off;
    MmapColumn<int32_t> mc_company;

    MmapColumn<int32_t> ci_off;
    MmapColumn<int32_t> ci_person;

    MmapColumn<int32_t> an_off;
    MmapColumn<int64_t> an_name_off;
    MmapColumn<char>    an_name_dat;

    MmapColumn<int64_t> t_off;
    MmapColumn<char>    t_dat;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(store + "/company_name/country_code.bin");
        cc_dict_off.open(store + "/company_name/country_code.dict.off");
        cc_dict_dat.open(store + "/company_name/country_code.dict.dat");

        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");

        mk_k_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_k_row.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie.open(store + "/movie_keyword/movie_id.bin");

        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company.open(store + "/movie_companies/company_id.bin");

        ci_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person.open(store + "/cast_info/person_id.bin");

        an_off.open(store + "/_idx/aka_name__person_id__offsets.bin");
        an_name_off.open(store + "/aka_name/name.off");
        an_name_dat.open(store + "/aka_name/name.dat");

        t_off.open(store + "/title/title.off");
        t_dat.open(store + "/title/title.dat");

        // Hint kernels: most are random-access in the hot loop.
        mc_off.advise_random();
        mc_company.advise_random();
        ci_off.advise_random();
        ci_person.advise_random();
        an_off.advise_random();
        an_name_off.advise_random();
        an_name_dat.advise_random();
        t_off.advise_random();
        t_dat.advise_random();
    }

    // --------------- Resolve us_code and k_id ----------------
    // dict.off has K+1 int64 entries → K = count-1 codes; code 0 = NULL, code i refers dict[i-1].
    int32_t us_code = 0;
    {
        size_t K = (cc_dict_off.count > 0) ? cc_dict_off.count - 1 : 0;
        int64_t idx = find_varlen_row(cc_dict_off.data, K, cc_dict_dat.data, p_country);
        if (idx < 0) {
            std::fprintf(stderr, "country_code '%s' not found in dict\n", p_country.c_str());
            return 1;
        }
        us_code = (int32_t)(idx + 1); // code i references dict[i-1] → us code = idx+1
    }

    int32_t k_id = 0;
    {
        size_t K = (kw_off.count > 0) ? kw_off.count - 1 : 0;
        int64_t row = find_varlen_row(kw_off.data, K, kw_dat.data, p_keyword);
        if (row < 0) {
            std::fprintf(stderr, "keyword '%s' not found\n", p_keyword.c_str());
            return 1;
        }
        // keyword.id is identity dense-PK → id = row + 1
        k_id = (int32_t)(row + 1);
    }

    // --------------- Build US company mask, indexed by cn.id (1-based, dense PK) ----------------
    const size_t cn_n = cn_cc.count; // 234997
    std::vector<uint8_t> us_company_mask(cn_n + 2, 0); // 1-based; +2 for safety
    {
        GENDB_PHASE("us_mask_build");
        for (size_t i = 0; i < cn_n; ++i) {
            if ((int32_t)cn_cc[i] == us_code) us_company_mask[i + 1] = 1;
        }
    }

    // --------------- CSR enumeration of movie_keyword for k_id → unique movie_ids ----------------
    const size_t title_n = (t_off.count > 0) ? t_off.count - 1 : 0; // 2528312
    std::vector<uint8_t> cand_mv(title_n + 2, 0);                   // 1-based bitmask
    {
        GENDB_PHASE("mk_csr_enum");
        int32_t lo = mk_k_off[k_id];
        int32_t hi = mk_k_off[k_id + 1];
        for (int32_t k = lo; k < hi; ++k) {
            int32_t r  = mk_k_row[k];
            int32_t mv = mk_movie[r];
            if (mv >= 1 && (size_t)mv <= title_n) cand_mv[mv] = 1;
        }
    }

    // --------------- Semi-join with US movie_companies; produce qualifying_movies[] ----------------
    std::vector<int32_t> qualifying_movies;
    qualifying_movies.reserve(100000);
    {
        GENDB_PHASE("us_semi_join");
        for (int32_t mv = 1; mv <= (int32_t)title_n; ++mv) {
            if (!cand_mv[mv]) continue;
            int32_t s = mc_off[mv];
            int32_t e = mc_off[mv + 1];
            bool has_us = false;
            for (int32_t j = s; j < e; ++j) {
                int32_t cid = mc_company[j];
                if (cid >= 1 && (size_t)cid <= cn_n && us_company_mask[cid]) {
                    has_us = true;
                    break;
                }
            }
            if (has_us) qualifying_movies.push_back(mv);
        }
    }

    // --------------- Parallel main scan: per movie → ci → aka_name; MIN aggregates ----------------
    int nthreads = (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 12;
    if ((int)qualifying_movies.size() < nthreads) nthreads = std::max(1, (int)qualifying_movies.size());

    std::vector<std::string> local_min_an(nthreads);
    std::vector<std::string> local_min_tt(nthreads);
    std::vector<uint8_t> local_has_an(nthreads, 0);
    std::vector<uint8_t> local_has_tt(nthreads, 0);

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        size_t total = qualifying_movies.size();

        auto worker = [&](int tid) {
            size_t chunk = (total + nthreads - 1) / nthreads;
            size_t a = (size_t)tid * chunk;
            size_t b = std::min(a + chunk, total);
            std::string best_an;  bool have_an = false;
            std::string best_tt;  bool have_tt = false;

            for (size_t mi = a; mi < b; ++mi) {
                int32_t mv = qualifying_movies[mi];
                int32_t cs = ci_off[mv];
                int32_t ce = ci_off[mv + 1];
                bool any_an = false;

                for (int32_t r = cs; r < ce; ++r) {
                    int32_t pid = ci_person[r];
                    if (pid < 1) continue;
                    int32_t as = an_off[pid];
                    int32_t ae = an_off[pid + 1];
                    if (as == ae) continue;
                    any_an = true;
                    for (int32_t j = as; j < ae; ++j) {
                        size_t lo = (size_t)an_name_off[j];
                        size_t hi = (size_t)an_name_off[j + 1];
                        const char* p = an_name_dat.data + lo;
                        size_t len = hi - lo;
                        std::string_view v(p, len);
                        if (!have_an) {
                            best_an.assign(p, len);
                            have_an = true;
                        } else {
                            std::string_view cur(best_an.data(), best_an.size());
                            if (v < cur) best_an.assign(p, len);
                        }
                    }
                }

                if (any_an) {
                    size_t tlo = (size_t)t_off[mv - 1];
                    size_t thi = (size_t)t_off[mv];
                    const char* p = t_dat.data + tlo;
                    size_t len = thi - tlo;
                    std::string_view v(p, len);
                    if (!have_tt) {
                        best_tt.assign(p, len);
                        have_tt = true;
                    } else {
                        std::string_view cur(best_tt.data(), best_tt.size());
                        if (v < cur) best_tt.assign(p, len);
                    }
                }
            }

            local_min_an[tid] = std::move(best_an);
            local_has_an[tid] = have_an ? 1 : 0;
            local_min_tt[tid] = std::move(best_tt);
            local_has_tt[tid] = have_tt ? 1 : 0;
        };

        for (int t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
        for (auto& th : threads) th.join();
    }

    // --------------- Reduce ----------------
    std::string final_an;  bool have_an = false;
    std::string final_tt;  bool have_tt = false;
    for (int t = 0; t < nthreads; ++t) {
        if (local_has_an[t]) {
            if (!have_an || local_min_an[t] < final_an) { final_an = local_min_an[t]; have_an = true; }
        }
        if (local_has_tt[t]) {
            if (!have_tt || local_min_tt[t] < final_tt) { final_tt = local_min_tt[t]; have_tt = true; }
        }
    }

    // --------------- Output ----------------
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q16b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s for writing\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "cool_actor_pseudonym,series_named_after_char\n");
        std::string a = have_an ? csv_quote(final_an) : std::string();
        std::string b = have_tt ? csv_quote(final_tt) : std::string();
        std::fprintf(f, "%s,%s\n", a.c_str(), b.c_str());
        std::fclose(f);
    }

    return 0;
}
