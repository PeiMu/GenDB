// Q11c - MIN(cn.name), MIN(mc.note), MIN(t.title)
// Plan: cn prefix '20th Century Fox%' / 'Twentieth Century Fox%' is the driver.
// Expand to mc rows via movie_companies__company_id CSR, then probe title row,
// then probe movie_keyword (keyword_id in K) and movie_link (existence).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <climits>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static std::string read_file_str(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    s.resize(sz);
    if (sz > 0) std::fread(&s[0], 1, sz, f);
    std::fclose(f);
    return s;
}

static void write_csv_field(std::string& out, std::string_view v) {
    // Determine if quoting is necessary
    bool need_quote = false;
    for (char c : v) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        out.append(v.data(), v.size());
        return;
    }
    out.push_back('"');
    for (char c : v) {
        if (c == '"') { out.push_back('"'); out.push_back('"'); }
        else out.push_back(c);
    }
    out.push_back('"');
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    mkdir(results_dir.c_str(), 0755);

    GENDB_PHASE("total");

    // ---- Resolve dictionary codes ----
    int16_t pl_code = 0;
    int32_t pc_id = 0;
    int32_t K[3]; int n_K = 0;

    // Keep these strings alive throughout the program
    static std::string cc_dat, ct_dat;

    // Resolve pl_code from company_name.country_code dict (looking for "[pl]")
    {
        auto cc_off_vec = read_file_str(store + "/company_name/country_code.dict.off");
        const int64_t* cc_off = reinterpret_cast<const int64_t*>(cc_off_vec.data());
        size_t cc_off_count = cc_off_vec.size() / sizeof(int64_t);
        cc_dat = read_file_str(store + "/company_name/country_code.dict.dat");
        for (size_t i = 0; i + 1 < cc_off_count; ++i) {
            std::string_view sv(cc_dat.data() + cc_off[i], cc_off[i+1] - cc_off[i]);
            if (sv == "[pl]") { pl_code = (int16_t)(i + 1); break; }
        }
    }

    // Resolve pc_id from company_type.kind (scan 4 rows)
    {
        auto ct_off_vec = read_file_str(store + "/company_type/kind.off");
        const int64_t* ct_off = reinterpret_cast<const int64_t*>(ct_off_vec.data());
        size_t ct_off_count = ct_off_vec.size() / sizeof(int64_t);
        ct_dat = read_file_str(store + "/company_type/kind.dat");
        for (size_t i = 0; i + 1 < ct_off_count; ++i) {
            std::string_view sv(ct_dat.data() + ct_off[i], ct_off[i+1] - ct_off[i]);
            if (sv == "production companies") { pc_id = (int32_t)(i + 1); break; }
        }
    }

    if (pl_code == 0) {
        std::fprintf(stderr, "warning: pl_code not resolved\n");
    }
    if (pc_id == 0) {
        std::fprintf(stderr, "warning: pc_id not resolved\n");
    }

    // Resolve keyword ids for 'sequel','revenge','based-on-novel'
    {
        MmapColumn<int64_t> kw_off;
        MmapColumn<char>    kw_dat;
        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");
        size_t nrows = kw_off.count > 0 ? kw_off.count - 1 : 0;
        const int64_t* off = kw_off.data;
        const char* dat = kw_dat.data;
        for (size_t i = 0; i < nrows && n_K < 3; ++i) {
            size_t len = (size_t)(off[i+1] - off[i]);
            const char* p = dat + off[i];
            if (len == 6 && std::memcmp(p, "sequel", 6) == 0) {
                K[n_K++] = (int32_t)(i + 1);
            } else if (len == 7 && std::memcmp(p, "revenge", 7) == 0) {
                K[n_K++] = (int32_t)(i + 1);
            } else if (len == 14 && std::memcmp(p, "based-on-novel", 14) == 0) {
                K[n_K++] = (int32_t)(i + 1);
            }
        }
    }

    if (n_K == 0) {
        std::fprintf(stderr, "warning: no keywords resolved\n");
    }

    // ---- mmap data columns ----
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int64_t> cn_name_off;
    MmapColumn<char>    cn_name_dat;

    MmapColumn<int32_t> mc_off_by_cid;  // CSR offsets[cn_id]
    MmapColumn<int32_t> mc_rows_by_cid; // rowids

    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_type_id;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<char>    mc_note_dat;

    MmapColumn<int32_t> t_production_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mk_off; // offsets per movie_id
    MmapColumn<int32_t> mk_keyword_id;

    MmapColumn<int32_t> ml_off; // offsets per movie_id

    {
        GENDB_PHASE("data_loading");
        cn_country_code.open(store + "/company_name/country_code.bin");
        cn_name_off.open(store + "/company_name/name.off");
        cn_name_dat.open(store + "/company_name/name.dat");

        mc_off_by_cid.open(store + "/_idx/movie_companies__company_id__offsets.bin");
        mc_rows_by_cid.open(store + "/_idx/movie_companies__company_id__rowids.bin");

        mc_movie_id.open(store + "/movie_companies/movie_id.bin");
        mc_company_type_id.open(store + "/movie_companies/company_type_id.bin");
        mc_note_off.open(store + "/movie_companies/note.off");
        mc_note_dat.open(store + "/movie_companies/note.dat");

        t_production_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        mk_off.open(store + "/_idx/movie_keyword__movie_id__offsets.bin");
        mk_keyword_id.open(store + "/movie_keyword/keyword_id.bin");

        ml_off.open(store + "/_idx/movie_link__movie_id__offsets.bin");

        // mc & title columns are accessed randomly via cn_id/mid
        mc_movie_id.advise_random();
        mc_company_type_id.advise_random();
        mc_note_off.advise_random();
        mc_note_dat.advise_random();
        t_production_year.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
        mk_off.advise_random();
        mk_keyword_id.advise_random();
        ml_off.advise_random();
    }

    // ---- Phase 1: scan company_name and find prefix matches ----
    std::vector<int32_t> cn_ids;
    cn_ids.reserve(32);

    static const char prefix1[] = "20th Century Fox";
    static const size_t prefix1_len = sizeof(prefix1) - 1; // 16
    static const char prefix2[] = "Twentieth Century Fox";
    static const size_t prefix2_len = sizeof(prefix2) - 1; // 21

    {
        GENDB_PHASE("cn_filter");
        const int64_t* __restrict__ name_off = cn_name_off.data;
        const char*    __restrict__ name_dat = cn_name_dat.data;
        const int16_t* __restrict__ cc = cn_country_code.data;
        size_t nrows = cn_name_off.count > 0 ? cn_name_off.count - 1 : 0;

        for (size_t i = 0; i < nrows; ++i) {
            // country_code filter first (cheap, dense)
            int16_t c = cc[i];
            if (c == 0 || c == pl_code) continue;

            int64_t o0 = name_off[i];
            int64_t o1 = name_off[i+1];
            size_t len = (size_t)(o1 - o0);
            const char* p = name_dat + o0;

            bool match = false;
            if (len >= prefix1_len && std::memcmp(p, prefix1, prefix1_len) == 0) {
                match = true;
            } else if (len >= prefix2_len && std::memcmp(p, prefix2, prefix2_len) == 0) {
                match = true;
            }
            if (!match) continue;

            cn_ids.push_back((int32_t)(i + 1));
        }
    }

    // ---- Phase 2: main scan — expand cn_ids -> mc rows -> probe title/mk/ml ----
    bool has_cn = false, has_mc = false, has_t = false;
    std::string_view min_cn, min_mc, min_t;

    {
        GENDB_PHASE("main_scan");

        const int32_t* __restrict__ mc_off_p   = mc_off_by_cid.data;
        const int32_t* __restrict__ mc_rows_p  = mc_rows_by_cid.data;
        const int32_t* __restrict__ mc_mid_p   = mc_movie_id.data;
        const int32_t* __restrict__ mc_ct_p    = mc_company_type_id.data;
        const int64_t* __restrict__ mc_note_off_p = mc_note_off.data;
        const char*    __restrict__ mc_note_dat_p = mc_note_dat.data;
        const int32_t* __restrict__ t_py_p     = t_production_year.data;
        const int64_t* __restrict__ t_title_off_p = t_title_off.data;
        const char*    __restrict__ t_title_dat_p = t_title_dat.data;
        const int32_t* __restrict__ mk_off_p   = mk_off.data;
        const int32_t* __restrict__ mk_kw_p    = mk_keyword_id.data;
        const int32_t* __restrict__ ml_off_p   = ml_off.data;
        const int64_t* __restrict__ cn_name_off_p = cn_name_off.data;
        const char*    __restrict__ cn_name_dat_p = cn_name_dat.data;

        for (int32_t cn_id : cn_ids) {
            int32_t mc_lo = mc_off_p[cn_id];
            int32_t mc_hi = mc_off_p[cn_id + 1];
            if (mc_lo >= mc_hi) continue;

            // Get cn.name view (potential MIN candidate)
            int64_t cno0 = cn_name_off_p[cn_id - 1];
            int64_t cno1 = cn_name_off_p[cn_id];
            std::string_view cn_name_sv(cn_name_dat_p + cno0, (size_t)(cno1 - cno0));

            for (int32_t k = mc_lo; k < mc_hi; ++k) {
                int32_t mc_row = mc_rows_p[k];

                // Filter: ct_id valid
                int32_t ctid = mc_ct_p[mc_row];
                if (ctid == 0 || ctid == pc_id) continue;

                // Filter: note non-empty
                int64_t no0 = mc_note_off_p[mc_row];
                int64_t no1 = mc_note_off_p[mc_row + 1];
                if (no0 == no1) continue;

                int32_t mid = mc_mid_p[mc_row];
                if (mid <= 0) continue;

                // title row = mid-1
                int32_t py = t_py_p[mid - 1];
                if (py == INT32_MIN || py <= 1950) continue;

                // probe movie_keyword: any row with keyword_id in K?
                int32_t mk_lo = mk_off_p[mid];
                int32_t mk_hi = mk_off_p[mid + 1];
                bool kw_match = false;
                for (int32_t j = mk_lo; j < mk_hi; ++j) {
                    int32_t kw = mk_kw_p[j];
                    for (int kk = 0; kk < n_K; ++kk) {
                        if (kw == K[kk]) { kw_match = true; break; }
                    }
                    if (kw_match) break;
                }
                if (!kw_match) continue;

                // probe movie_link: existence
                int32_t ml_lo = ml_off_p[mid];
                int32_t ml_hi = ml_off_p[mid + 1];
                if (ml_hi <= ml_lo) continue;

                // Row qualifies. Update MINs.
                if (!has_cn || cn_name_sv < min_cn) {
                    min_cn = cn_name_sv;
                    has_cn = true;
                }
                {
                    std::string_view note_sv(mc_note_dat_p + no0, (size_t)(no1 - no0));
                    if (!has_mc || note_sv < min_mc) {
                        min_mc = note_sv;
                        has_mc = true;
                    }
                }
                {
                    int64_t to0 = t_title_off_p[mid - 1];
                    int64_t to1 = t_title_off_p[mid];
                    std::string_view title_sv(t_title_dat_p + to0, (size_t)(to1 - to0));
                    if (!has_t || title_sv < min_t) {
                        min_t = title_sv;
                        has_t = true;
                    }
                }
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q11c.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::string row;
        row.reserve(1024);
        row.append("from_company,production_note,movie_based_on_book\n");
        if (has_cn) write_csv_field(row, min_cn);
        row.push_back(',');
        if (has_mc) write_csv_field(row, min_mc);
        row.push_back(',');
        if (has_t) write_csv_field(row, min_t);
        row.push_back('\n');
        std::fwrite(row.data(), 1, row.size(), f);
        std::fclose(f);
    }

    return 0;
}
