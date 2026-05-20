// Q21b: MIN(cn.name), MIN(lt.link), MIN(t.title)
// German follow-up movies (BETWEEN 2000 AND 2010), sequel keyword, follow-link types,
// production-companies type, non-PL company with name LIKE %Film% or %Warner%.
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static std::string path_join(const std::string& a, const std::string& b) {
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

static inline bool slice_contains(const char* hay, size_t hay_len,
                                  const char* needle, size_t n_len) {
    if (n_len == 0) return true;
    if (hay_len < n_len) return false;
    return memmem(hay, hay_len, needle, n_len) != nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--param ...]\n", argv[0]);
        return 1;
    }
    init_date_tables();
    const std::string gendb_dir   = argv[1];
    const std::string results_dir = argv[2];

    // Parameterized literals (defaults per Q21b SQL)
    std::string p_ct_kind = parse_string_arg(argc, argv, "--ct_kind", std::string("production companies"));
    std::string p_keyword = parse_string_arg(argc, argv, "--keyword", std::string("sequel"));
    std::string p_lt_pat  = parse_string_arg(argc, argv, "--lt_pat",  std::string("follow"));
    std::string p_cc_excl = parse_string_arg(argc, argv, "--cc_excl", std::string("[pl]"));
    std::string p_cn_pat1 = parse_string_arg(argc, argv, "--cn_pat1", std::string("Film"));
    std::string p_cn_pat2 = parse_string_arg(argc, argv, "--cn_pat2", std::string("Warner"));
    std::string p_mi_v1   = parse_string_arg(argc, argv, "--mi_v1",   std::string("Germany"));
    std::string p_mi_v2   = parse_string_arg(argc, argv, "--mi_v2",   std::string("German"));
    int32_t p_year_lo = (int32_t)parse_int_arg(argc, argv, "--year_lo", 2000);
    int32_t p_year_hi = (int32_t)parse_int_arg(argc, argv, "--year_hi", 2010);

    {
        std::string mkcmd = "mkdir -p '" + results_dir + "'";
        (void)system(mkcmd.c_str());
    }

    GENDB_PHASE("total");

    // ---- mmap all required files ----
    // Dimension columns
    MmapColumn<int64_t> ct_kind_off, k_kw_off, lt_link_off, cn_name_off;
    MmapColumn<char>    ct_kind_dat, k_kw_dat, lt_link_dat, cn_name_dat;
    MmapColumn<int64_t> cc_dict_off;
    MmapColumn<char>    cc_dict_dat;
    MmapColumn<int16_t> cn_country_code; // dict codes

    // Title
    MmapColumn<int32_t> t_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    // movie_link
    MmapColumn<int32_t> ml_movie_id;
    MmapColumn<int32_t> ml_lt_off_idx;     // CSR offsets (int32, parent_max+2)
    MmapColumn<int32_t> ml_lt_rowids;      // CSR rowids

    // movie_keyword
    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mk_kw_off_idx;     // CSR offsets
    MmapColumn<int32_t> mk_kw_rowids;      // CSR rowids

    // movie_info (info IN list); offsets_only on movie_id
    MmapColumn<int32_t> mi_mid_off_idx;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;

    // movie_companies (note IS NULL, company_type_id, company_id); offsets_only on movie_id
    MmapColumn<int32_t> mc_mid_off_idx;
    MmapColumn<int32_t> mc_company_type_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int64_t> mc_note_off;

    {
        GENDB_PHASE("data_loading");
        ct_kind_off.open(path_join(gendb_dir, "company_type/kind.off"));
        ct_kind_dat.open(path_join(gendb_dir, "company_type/kind.dat"));

        k_kw_off.open(path_join(gendb_dir, "keyword/keyword.off"));
        k_kw_dat.open(path_join(gendb_dir, "keyword/keyword.dat"));

        lt_link_off.open(path_join(gendb_dir, "link_type/link.off"));
        lt_link_dat.open(path_join(gendb_dir, "link_type/link.dat"));

        cn_name_off.open(path_join(gendb_dir, "company_name/name.off"));
        cn_name_dat.open(path_join(gendb_dir, "company_name/name.dat"));
        cc_dict_off.open(path_join(gendb_dir, "company_name/country_code.dict.off"));
        cc_dict_dat.open(path_join(gendb_dir, "company_name/country_code.dict.dat"));
        cn_country_code.open(path_join(gendb_dir, "company_name/country_code.bin"));

        t_year.open(path_join(gendb_dir, "title/production_year.bin"));
        t_title_off.open(path_join(gendb_dir, "title/title.off"));
        t_title_dat.open(path_join(gendb_dir, "title/title.dat"));

        ml_movie_id.open(path_join(gendb_dir, "movie_link/movie_id.bin"));
        ml_lt_off_idx.open(path_join(gendb_dir, "_idx/movie_link__link_type_id__offsets.bin"));
        ml_lt_rowids.open(path_join(gendb_dir, "_idx/movie_link__link_type_id__rowids.bin"));

        mk_movie_id.open(path_join(gendb_dir, "movie_keyword/movie_id.bin"));
        mk_kw_off_idx.open(path_join(gendb_dir, "_idx/movie_keyword__keyword_id__offsets.bin"));
        mk_kw_rowids.open(path_join(gendb_dir, "_idx/movie_keyword__keyword_id__rowids.bin"));

        mi_mid_off_idx.open(path_join(gendb_dir, "_idx/movie_info__movie_id__offsets.bin"));
        mi_info_off.open(path_join(gendb_dir, "movie_info/info.off"));
        mi_info_dat.open(path_join(gendb_dir, "movie_info/info.dat"));

        mc_mid_off_idx.open(path_join(gendb_dir, "_idx/movie_companies__movie_id__offsets.bin"));
        mc_company_type_id.open(path_join(gendb_dir, "movie_companies/company_type_id.bin"));
        mc_company_id.open(path_join(gendb_dir, "movie_companies/company_id.bin"));
        mc_note_off.open(path_join(gendb_dir, "movie_companies/note.off"));

        // Prefetch large randomly accessed arrays
        t_year.prefetch();
        t_title_off.prefetch();
        mc_mid_off_idx.prefetch();
        mi_mid_off_idx.prefetch();
        mc_company_type_id.prefetch();
        mc_company_id.prefetch();
        mc_note_off.prefetch();
        mi_info_off.prefetch();
        mk_movie_id.prefetch();
    }

    // ---- Resolve dimension scalars / bitsets ----
    // company_type
    int32_t ct_pc = -1;
    {
        size_t n = ct_kind_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = ct_kind_off.data[i];
            int64_t hi = ct_kind_off.data[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len == p_ct_kind.size() &&
                std::memcmp(ct_kind_dat.data + lo, p_ct_kind.data(), len) == 0) {
                ct_pc = (int32_t)(i + 1);
                break;
            }
        }
    }

    // keyword
    int32_t k_sequel = -1;
    {
        size_t n = k_kw_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = k_kw_off.data[i];
            int64_t hi = k_kw_off.data[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len == p_keyword.size() &&
                std::memcmp(k_kw_dat.data + lo, p_keyword.data(), len) == 0) {
                k_sequel = (int32_t)(i + 1);
                break;
            }
        }
    }

    // link_type ids matching LIKE %p_lt_pat%
    std::vector<int32_t> LT_ids;
    std::vector<std::string_view> LT_links;
    {
        size_t n = lt_link_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = lt_link_off.data[i];
            int64_t hi = lt_link_off.data[i + 1];
            size_t len = (size_t)(hi - lo);
            const char* s = lt_link_dat.data + lo;
            if (slice_contains(s, len, p_lt_pat.data(), p_lt_pat.size())) {
                LT_ids.push_back((int32_t)(i + 1));
                LT_links.emplace_back(s, len);
            }
        }
    }

    // country_code dict: find code for p_cc_excl
    int16_t cc_pl = -1;
    {
        size_t n = cc_dict_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = cc_dict_off.data[i];
            int64_t hi = cc_dict_off.data[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len == p_cc_excl.size() &&
                std::memcmp(cc_dict_dat.data + lo, p_cc_excl.data(), len) == 0) {
                cc_pl = (int16_t)(i + 1); // dict code i references dict entry i-1 -> code = i+1 for 0-based i
                break;
            }
        }
    }

    // Build CN_ids bitset + per-cn-id name string_view for MIN
    const size_t n_cn = cn_country_code.count; // 234997
    std::vector<uint64_t> CN_bits((n_cn + 63) / 64, 0);
    // We don't precompute a name array — we'll look up via offsets on demand.
    {
        GENDB_PHASE("cn_filter");
        const int16_t* cc = cn_country_code.data;
        const int64_t* nm_off = cn_name_off.data;
        const char*    nm_dat = cn_name_dat.data;
        for (size_t i = 0; i < n_cn; ++i) {
            int16_t c = cc[i];
            if (c == 0) continue;           // NULL — fails != predicate
            if (c == cc_pl) continue;       // != '[pl]'
            int64_t lo = nm_off[i];
            int64_t hi = nm_off[i + 1];
            size_t len = (size_t)(hi - lo);
            const char* s = nm_dat + lo;
            bool match = slice_contains(s, len, p_cn_pat1.data(), p_cn_pat1.size())
                      || slice_contains(s, len, p_cn_pat2.data(), p_cn_pat2.size());
            if (match) {
                CN_bits[i >> 6] |= (uint64_t)1 << (i & 63);
            }
        }
    }

    // Build seq_movies bitset (size = title.rows)
    const size_t n_title = (t_title_off.count > 0) ? (t_title_off.count - 1) : 0;
    std::vector<uint64_t> seq_bits((n_title + 63) / 64, 0);
    if (k_sequel > 0) {
        GENDB_PHASE("build_seq_bitset");
        int32_t lo = mk_kw_off_idx.data[k_sequel];
        int32_t hi = mk_kw_off_idx.data[k_sequel + 1];
        const int32_t* rowids = mk_kw_rowids.data;
        const int32_t* mids   = mk_movie_id.data;
        for (int32_t r = lo; r < hi; ++r) {
            int32_t row = rowids[r];
            int32_t mv  = mids[row];
            if (mv <= 0) continue;
            size_t idx = (size_t)(mv - 1);
            if (idx >= n_title) continue;
            seq_bits[idx >> 6] |= (uint64_t)1 << (idx & 63);
        }
    }

    // Collect candidate (mv, lt_index) pairs from movie_link
    struct Cand { int32_t mv; int32_t lt_idx; };
    std::vector<Cand> cands;
    cands.reserve(8192);
    {
        GENDB_PHASE("collect_ml");
        const int32_t* ml_off = ml_lt_off_idx.data;
        const int32_t* ml_row = ml_lt_rowids.data;
        const int32_t* ml_mid = ml_movie_id.data;
        for (size_t li = 0; li < LT_ids.size(); ++li) {
            int32_t lt = LT_ids[li];
            int32_t lo = ml_off[lt];
            int32_t hi = ml_off[lt + 1];
            for (int32_t r = lo; r < hi; ++r) {
                int32_t row = ml_row[r];
                int32_t mv  = ml_mid[row];
                cands.push_back({mv, (int32_t)li});
            }
        }
    }

    // Aggregation holders
    bool has_cn = false; std::string_view min_cn_name;
    bool has_lt = false; std::string_view min_lt_link;
    bool has_t  = false; std::string_view min_t_title;

    // String set probe helper: check if a slice equals one of the two MI values.
    const char* mi_a = p_mi_v1.data(); const size_t mi_a_len = p_mi_v1.size();
    const char* mi_b = p_mi_v2.data(); const size_t mi_b_len = p_mi_v2.size();

    {
        GENDB_PHASE("main_scan");
        const int32_t* t_year_p   = t_year.data;
        const int64_t* t_title_o  = t_title_off.data;
        const char*    t_title_d  = t_title_dat.data;
        const int32_t* mi_off     = mi_mid_off_idx.data;
        const int64_t* mi_info_o  = mi_info_off.data;
        const char*    mi_info_d  = mi_info_dat.data;
        const int32_t* mc_off     = mc_mid_off_idx.data;
        const int32_t* mc_ct_p    = mc_company_type_id.data;
        const int32_t* mc_cid_p   = mc_company_id.data;
        const int64_t* mc_note_o  = mc_note_off.data;
        const int64_t* cn_nm_o    = cn_name_off.data;
        const char*    cn_nm_d    = cn_name_dat.data;

        for (const auto& c : cands) {
            int32_t mv = c.mv;
            if (mv <= 0 || (size_t)(mv - 1) >= n_title) continue;

            // year filter
            int32_t yr = t_year_p[mv - 1];
            if (yr == INT32_MIN) continue;
            if (yr < p_year_lo || yr > p_year_hi) continue;

            // seq_movies
            size_t bit_idx = (size_t)(mv - 1);
            if (!(seq_bits[bit_idx >> 6] & ((uint64_t)1 << (bit_idx & 63)))) continue;

            // movie_info: info IN MI_set (semi join)
            int32_t mi_lo = mi_off[mv];
            int32_t mi_hi = mi_off[mv + 1];
            bool mi_ok = false;
            for (int32_t r = mi_lo; r < mi_hi; ++r) {
                int64_t lo = mi_info_o[r];
                int64_t hi = mi_info_o[r + 1];
                size_t len = (size_t)(hi - lo);
                if (len == mi_a_len && std::memcmp(mi_info_d + lo, mi_a, mi_a_len) == 0) { mi_ok = true; break; }
                if (len == mi_b_len && std::memcmp(mi_info_d + lo, mi_b, mi_b_len) == 0) { mi_ok = true; break; }
            }
            if (!mi_ok) continue;

            // movie_companies inner join with cn (note empty, ct match, cn match)
            int32_t mc_lo = mc_off[mv];
            int32_t mc_hi = mc_off[mv + 1];
            bool any_mc = false;
            for (int32_t r = mc_lo; r < mc_hi; ++r) {
                if (mc_ct_p[r] != ct_pc) continue;
                int64_t nlo = mc_note_o[r];
                int64_t nhi = mc_note_o[r + 1];
                if (nhi != nlo) continue; // note IS NULL (empty string)
                int32_t cid = mc_cid_p[r];
                if (cid <= 0 || (size_t)(cid - 1) >= n_cn) continue;
                size_t ci = (size_t)(cid - 1);
                if (!(CN_bits[ci >> 6] & ((uint64_t)1 << (ci & 63)))) continue;

                // surviving cn — update MIN(cn.name)
                int64_t lo2 = cn_nm_o[ci];
                int64_t hi2 = cn_nm_o[ci + 1];
                std::string_view cn_view(cn_nm_d + lo2, (size_t)(hi2 - lo2));
                if (!has_cn || cn_view < min_cn_name) {
                    has_cn = true; min_cn_name = cn_view;
                }
                any_mc = true;
            }
            if (!any_mc) continue;

            // Tuple survives: update MIN(lt.link) and MIN(t.title)
            std::string_view lv = LT_links[c.lt_idx];
            if (!has_lt || lv < min_lt_link) { has_lt = true; min_lt_link = lv; }

            int64_t tlo = t_title_o[mv - 1];
            int64_t thi = t_title_o[mv];
            std::string_view tv(t_title_d + tlo, (size_t)(thi - tlo));
            if (!has_t || tv < min_t_title) { has_t = true; min_t_title = tv; }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = path_join(results_dir, "Q21b.csv");
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 2;
        }
        std::fputs("company_name,link_type,german_follow_up\n", f);

        auto write_csv_field = [&](std::string_view s) {
            bool need_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (!need_quote) {
                std::fwrite(s.data(), 1, s.size(), f);
            } else {
                std::fputc('"', f);
                for (char c : s) {
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            }
        };

        if (has_cn) write_csv_field(min_cn_name);
        std::fputc(',', f);
        if (has_lt) write_csv_field(min_lt_link);
        std::fputc(',', f);
        if (has_t)  write_csv_field(min_t_title);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
