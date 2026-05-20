// Q12c — title-driven plan with per-mid offsets indexes on mi_idx/mi/mc.
// Aggregation: MIN(cn.name), MIN(mi_idx.info), MIN(t.title).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <omp.h>
#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline std::string_view sv_from(const char* dat, const uint64_t* off, size_t i) {
    return std::string_view(dat + off[i], off[i+1] - off[i]);
}

// Lex compare: a > b
static inline bool sv_gt(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    int c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c > 0;
    return a.size() > b.size();
}

static void csv_escape(const std::string_view& v, std::string& out) {
    bool need = false;
    for (char c : v) if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
    if (!need) { out.append(v.data(), v.size()); return; }
    out.push_back('"');
    for (char c : v) {
        if (c == '"') { out.append("\"\""); }
        else out.push_back(c);
    }
    out.push_back('"');
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) { std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]); return 1; }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // --- mmap columns ---
    MmapColumn<int32_t>  t_id_col, t_py_col;
    MmapColumn<char>     t_title_dat;
    MmapColumn<uint64_t> t_title_off;

    MmapColumn<int32_t>  mii_movie_id, mii_info_type_id;
    MmapColumn<char>     mii_info_dat;
    MmapColumn<uint64_t> mii_info_off;

    MmapColumn<int32_t>  mi_movie_id, mi_info_type_id;
    MmapColumn<char>     mi_info_dat;
    MmapColumn<uint64_t> mi_info_off;

    MmapColumn<int32_t>  mc_movie_id, mc_company_id, mc_company_type_id;

    MmapColumn<int16_t>  cn_country_code;
    MmapColumn<char>     cn_name_dat, cn_cc_dict_dat;
    MmapColumn<uint64_t> cn_name_off, cn_cc_dict_off;

    MmapColumn<char>     ct_kind_dat;
    MmapColumn<uint64_t> ct_kind_off;
    MmapColumn<int32_t>  ct_id;

    MmapColumn<char>     it_info_dat;
    MmapColumn<uint64_t> it_info_off;
    MmapColumn<int32_t>  it_id;

    MmapColumn<int32_t>  mii_off_idx, mi_off_idx, mc_off_idx;

    {
        GENDB_PHASE("data_loading");
        t_id_col.open(gendb_dir + "/title/id.bin");
        t_py_col.open(gendb_dir + "/title/production_year.bin");
        t_title_dat.open(gendb_dir + "/title/title.dat");
        t_title_off.open(gendb_dir + "/title/title.off");

        mii_movie_id.open(gendb_dir + "/movie_info_idx/movie_id.bin");
        mii_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mii_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");
        mii_info_off.open(gendb_dir + "/movie_info_idx/info.off");

        mi_movie_id.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");

        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");

        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");
        cn_cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");

        ct_kind_dat.open(gendb_dir + "/company_type/kind.dat");
        ct_kind_off.open(gendb_dir + "/company_type/kind.off");
        ct_id.open(gendb_dir + "/company_type/id.bin");

        it_info_dat.open(gendb_dir + "/info_type/info.dat");
        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_id.open(gendb_dir + "/info_type/id.bin");

        mii_off_idx.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");

        // hint random access for offset-driven probes
        mii_movie_id.advise_random();
        mii_info_type_id.advise_random();
        mii_info_dat.advise_random();
        mii_info_off.advise_random();
        mi_movie_id.advise_random();
        mi_info_type_id.advise_random();
        mi_info_dat.advise_random();
        mi_info_off.advise_random();
        mc_movie_id.advise_random();
        mc_company_id.advise_random();
        mc_company_type_id.advise_random();
        cn_country_code.advise_random();
        cn_name_dat.advise_random();
        cn_name_off.advise_random();

        // sequential on title scan + offset arrays
        t_py_col.advise_sequential();
        t_id_col.advise_sequential();
    }

    // --- Resolve dimensions ---
    int16_t us_code = -1;
    {
        size_t n = cn_cc_dict_off.count - 1;
        const std::string_view target("[us]");
        for (size_t i = 0; i < n; ++i) {
            std::string_view s(cn_cc_dict_dat.data + cn_cc_dict_off[i],
                               cn_cc_dict_off[i+1] - cn_cc_dict_off[i]);
            // dict code 0 = NULL; real codes start at 1 (code i = dict entry i-1)
            if (s == target) { us_code = (int16_t)(i + 1); break; }
        }
        if (us_code < 0) { std::fprintf(stderr, "country code [us] not found\n"); return 2; }
    }

    int32_t ct_prod_id = -1;
    {
        size_t n = ct_kind_off.count - 1;
        const std::string_view target("production companies");
        for (size_t i = 0; i < n; ++i) {
            std::string_view s(ct_kind_dat.data + ct_kind_off[i],
                               ct_kind_off[i+1] - ct_kind_off[i]);
            if (s == target) { ct_prod_id = ct_id[i]; break; }
        }
        if (ct_prod_id < 0) { std::fprintf(stderr, "company_type 'production companies' not found\n"); return 2; }
    }

    int32_t it1_id = -1, it2_id = -1;
    {
        size_t n = it_info_off.count - 1;
        const std::string_view t1("genres"), t2("rating");
        for (size_t i = 0; i < n; ++i) {
            std::string_view s(it_info_dat.data + it_info_off[i],
                               it_info_off[i+1] - it_info_off[i]);
            if (s == t1) it1_id = it_id[i];
            else if (s == t2) it2_id = it_id[i];
        }
        if (it1_id < 0 || it2_id < 0) { std::fprintf(stderr, "info_type genres/rating not found\n"); return 2; }
    }

    // Genres set: just check 4 strings directly by length+memcmp.
    auto in_genres = [](std::string_view v) -> bool {
        const char* p = v.data();
        switch (v.size()) {
            case 5: return std::memcmp(p, "Drama", 5) == 0;
            case 6: return std::memcmp(p, "Horror", 6) == 0 || std::memcmp(p, "Family", 6) == 0;
            case 7: return std::memcmp(p, "Western", 7) == 0;
            default: return false;
        }
    };

    const std::string_view RATING_GT("7.0");

    // --- Main scan: title-driven ---
    size_t n_titles = t_py_col.count;
    int n_threads = omp_get_max_threads();
    if (n_threads <= 0) n_threads = 1;

    struct MinAgg {
        bool has_name = false, has_rating = false, has_title = false;
        std::string_view name, rating, title;
    };
    std::vector<MinAgg> per_thread(n_threads);

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            MinAgg local;

            #pragma omp for schedule(static)
            for (size_t i = 0; i < n_titles; ++i) {
                int32_t py = t_py_col[i];
                if (py < 2000 || py > 2010) continue;

                int32_t mid = t_id_col[i];

                // -- mi_idx probe: find qualifying rows, capture best mi_idx.info for this mid
                int32_t lo = mii_off_idx[mid];
                int32_t hi = mii_off_idx[mid + 1];
                bool has_mii = false;
                std::string_view best_mii;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mii_info_type_id[r] != it2_id) continue;
                    std::string_view v(mii_info_dat.data + mii_info_off[r],
                                       mii_info_off[r+1] - mii_info_off[r]);
                    // info > '7.0' lex strict
                    if (!sv_gt(v, RATING_GT)) continue;
                    if (!has_mii || v < best_mii) {
                        best_mii = v;
                        has_mii = true;
                    }
                }
                if (!has_mii) continue;

                // -- mi probe: need at least one genre match
                lo = mi_off_idx[mid];
                hi = mi_off_idx[mid + 1];
                bool has_mi = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mi_info_type_id[r] != it1_id) continue;
                    std::string_view v(mi_info_dat.data + mi_info_off[r],
                                       mi_info_off[r+1] - mi_info_off[r]);
                    if (in_genres(v)) { has_mi = true; break; }
                }
                if (!has_mi) continue;

                // -- mc probe: filter ct + us check; capture min cn.name
                lo = mc_off_idx[mid];
                hi = mc_off_idx[mid + 1];
                bool any_mc = false;
                std::string_view best_cn;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mc_company_type_id[r] != ct_prod_id) continue;
                    int32_t cid = mc_company_id[r];
                    if (cn_country_code[cid - 1] != us_code) continue;
                    std::string_view nm(cn_name_dat.data + cn_name_off[cid - 1],
                                        cn_name_off[cid] - cn_name_off[cid - 1]);
                    if (!any_mc || nm < best_cn) {
                        best_cn = nm;
                        any_mc = true;
                    }
                }
                if (!any_mc) continue;

                // -- this mid produces tuples; update MINs
                std::string_view tt = sv_from(t_title_dat.data, t_title_off.data, i);

                if (!local.has_title || tt < local.title) { local.title = tt; local.has_title = true; }
                if (!local.has_rating || best_mii < local.rating) { local.rating = best_mii; local.has_rating = true; }
                if (!local.has_name || best_cn < local.name) { local.name = best_cn; local.has_name = true; }
            }

            per_thread[tid] = local;
        }
    }

    // Reduce per-thread mins
    MinAgg final_;
    for (auto& m : per_thread) {
        if (m.has_title && (!final_.has_title || m.title < final_.title)) { final_.title = m.title; final_.has_title = true; }
        if (m.has_rating && (!final_.has_rating || m.rating < final_.rating)) { final_.rating = m.rating; final_.has_rating = true; }
        if (m.has_name && (!final_.has_name || m.name < final_.name)) { final_.name = m.name; final_.has_name = true; }
    }

    // --- Output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q12c.csv";
        std::ofstream out(out_path);
        out << "movie_company,rating,mainstream_movie\n";
        std::string line;
        if (final_.has_name) csv_escape(final_.name, line);
        line.push_back(',');
        if (final_.has_rating) csv_escape(final_.rating, line);
        line.push_back(',');
        if (final_.has_title) csv_escape(final_.title, line);
        line.push_back('\n');
        out.write(line.data(), line.size());
    }

    return 0;
}
