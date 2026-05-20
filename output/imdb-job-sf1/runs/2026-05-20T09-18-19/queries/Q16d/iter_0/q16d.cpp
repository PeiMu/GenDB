// Q16d — MIN(an.name), MIN(t.title)
//
// Driver: CSR slice of movie_keyword keyed on keyword.id for 'character-name-in-title'.
// Filters: t.episode_nr in [5,100); company_name.country_code = '[us]'.
// Aggregation: scalar min over two varlen columns.

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using gendb::MmapColumn;

static inline std::string_view varlen_at(const uint64_t* off, const char* dat, size_t i) {
    uint64_t a = off[i], b = off[i + 1];
    return std::string_view(dat + a, b - a);
}

// CSV-escape a value (always wrap in quotes if it contains comma, quote, or newline;
// to match the validator we conditionally quote per RFC4180).
static std::string csv_escape(std::string_view s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
    }
    if (!needs_quote) return std::string(s);
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
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ------------------------------------------------------------------ load
    MmapColumn<int16_t> cn_cc;
    MmapColumn<uint64_t> cn_dict_off;
    MmapColumn<char>     cn_dict_dat;

    MmapColumn<uint64_t> kw_off;
    MmapColumn<char>     kw_dat;

    MmapColumn<int32_t>  t_ep;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int32_t>  mk_k_off;
    MmapColumn<int32_t>  mk_k_row;
    MmapColumn<int32_t>  mk_movie_id;

    MmapColumn<int32_t>  mc_off;
    MmapColumn<int32_t>  mc_company_id;

    MmapColumn<int32_t>  ci_off;
    MmapColumn<int32_t>  ci_person_id;

    MmapColumn<int32_t>  an_off;
    MmapColumn<uint64_t> an_name_off;
    MmapColumn<char>     an_name_dat;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(store + "/company_name/country_code.bin");
        cn_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_dict_dat.open(store + "/company_name/country_code.dict.dat");

        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");

        t_ep.open(store + "/title/episode_nr.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        mk_k_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_k_row.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");

        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");

        ci_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person_id.open(store + "/cast_info/person_id.bin");

        an_off.open(store + "/_idx/aka_name__person_id__offsets.bin");
        an_name_off.open(store + "/aka_name/name.off");
        an_name_dat.open(store + "/aka_name/name.dat");
    }

    // ----------------------------------------------------- resolve us_code
    int16_t us_code = -1;
    {
        GENDB_PHASE("resolve_us_code");
        size_t n_codes = cn_dict_off.size() - 1;
        const char* dat = cn_dict_dat.data;
        const uint64_t* off = cn_dict_off.data;
        std::string_view target("[us]");
        for (size_t i = 0; i < n_codes; ++i) {
            uint64_t a = off[i], b = off[i + 1];
            std::string_view s(dat + a, b - a);
            if (s == target) { us_code = (int16_t)i; break; }
        }
        if (us_code < 0) {
            std::fprintf(stderr, "us_code not found\n");
            return 2;
        }
    }

    // ----------------------------------------------------- resolve k_id
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_k_id");
        size_t n_kw = kw_off.size() - 1;
        const char* dat = kw_dat.data;
        const uint64_t* off = kw_off.data;
        std::string_view target("character-name-in-title");
        for (size_t i = 0; i < n_kw; ++i) {
            uint64_t a = off[i], b = off[i + 1];
            std::string_view s(dat + a, b - a);
            if (s == target) { k_id = (int32_t)(i + 1); break; } // 1-based keyword.id
        }
        if (k_id < 0) {
            std::fprintf(stderr, "k_id not found\n");
            return 3;
        }
    }

    // --------------------------- build cn_us_bs over company_name rowid
    std::vector<uint64_t> cn_us_bs;
    {
        GENDB_PHASE("build_cn_us_bitset");
        size_t n = cn_cc.size();
        cn_us_bs.assign((n + 63) / 64, 0);
        const int16_t* cc = cn_cc.data;
        for (size_t r = 0; r < n; ++r) {
            if (cc[r] == us_code) cn_us_bs[r >> 6] |= (1ULL << (r & 63));
        }
    }

    // ----------------------------- parallel scan of mk CSR slice for k_id
    int32_t lo = mk_k_off[k_id];
    int32_t hi = mk_k_off[k_id + 1];

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if ((int32_t)nthreads > (hi - lo)) nthreads = (unsigned)std::max(1, hi - lo);

    struct Best {
        std::string_view name;  // empty == sentinel "no value"
        bool name_set = false;
        std::string_view title;
        bool title_set = false;
    };
    std::vector<Best> per_thread(nthreads);

    auto worker = [&](unsigned tid, int32_t r_lo, int32_t r_hi) {
        Best& b = per_thread[tid];
        const int32_t* mk_row = mk_k_row.data;
        const int32_t* mk_mv  = mk_movie_id.data;
        const int32_t* mc_o = mc_off.data;
        const int32_t* mc_co = mc_company_id.data;
        const int32_t* ci_o = ci_off.data;
        const int32_t* ci_pi = ci_person_id.data;
        const int32_t* an_o = an_off.data;
        const uint64_t* an_no = an_name_off.data;
        const char*     an_nd = an_name_dat.data;
        const uint64_t* t_to  = t_title_off.data;
        const char*     t_td  = t_title_dat.data;
        const int32_t*  t_e   = t_ep.data;
        const uint64_t* bs    = cn_us_bs.data();

        for (int32_t k = r_lo; k < r_hi; ++k) {
            int32_t mk_rowid = mk_row[k];
            int32_t mv = mk_mv[mk_rowid];
            int32_t mv_idx = mv - 1;

            // title.episode_nr filter
            int32_t ep = t_e[mv_idx];
            if (ep == INT32_MIN || ep < 5 || ep >= 100) continue;

            // movie_companies semijoin (US)
            int32_t mc_lo = mc_o[mv_idx];
            int32_t mc_hi = mc_o[mv_idx + 1];
            bool us_ok = false;
            for (int32_t j = mc_lo; j < mc_hi; ++j) {
                int32_t cid = mc_co[j] - 1; // 0-based company_name rowid
                if (cid >= 0 && (bs[(uint32_t)cid >> 6] & (1ULL << (cid & 63)))) {
                    us_ok = true; break;
                }
            }
            if (!us_ok) continue;

            // update min(t.title)
            {
                std::string_view tt(t_td + t_to[mv_idx], t_to[mv_idx + 1] - t_to[mv_idx]);
                if (!b.title_set || tt < b.title) { b.title = tt; b.title_set = true; }
            }

            // walk ci rows for this movie
            int32_t ci_lo = ci_o[mv_idx];
            int32_t ci_hi = ci_o[mv_idx + 1];
            for (int32_t j = ci_lo; j < ci_hi; ++j) {
                int32_t pid = ci_pi[j];
                int32_t an_lo = an_o[pid - 1];
                int32_t an_hi = an_o[pid];
                for (int32_t a = an_lo; a < an_hi; ++a) {
                    std::string_view sn(an_nd + an_no[a], an_no[a + 1] - an_no[a]);
                    if (!b.name_set || sn < b.name) { b.name = sn; b.name_set = true; }
                }
            }
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        int32_t span = hi - lo;
        for (unsigned t = 0; t < nthreads; ++t) {
            int32_t r_lo = lo + (int32_t)((int64_t)span * t / nthreads);
            int32_t r_hi = lo + (int32_t)((int64_t)span * (t + 1) / nthreads);
            threads.emplace_back(worker, t, r_lo, r_hi);
        }
        for (auto& th : threads) th.join();
    }

    // --------------------------- reduce
    Best final_b;
    for (auto& b : per_thread) {
        if (b.name_set && (!final_b.name_set || b.name < final_b.name)) {
            final_b.name = b.name; final_b.name_set = true;
        }
        if (b.title_set && (!final_b.title_set || b.title < final_b.title)) {
            final_b.title = b.title; final_b.title_set = true;
        }
    }

    // --------------------------- write CSV
    {
        GENDB_PHASE("output");
        std::ofstream out(results_dir + "/Q16d.csv");
        out << "cool_actor_pseudonym,series_named_after_char\n";
        std::string col1 = final_b.name_set ? csv_escape(final_b.name) : std::string();
        std::string col2 = final_b.title_set ? csv_escape(final_b.title) : std::string();
        out << col1 << "," << col2 << "\n";
    }

    return 0;
}
