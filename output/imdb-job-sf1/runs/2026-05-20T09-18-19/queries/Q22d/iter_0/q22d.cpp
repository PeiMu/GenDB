// Q22d - JOB benchmark
// SELECT MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// Drive on title; per-title probes mk -> mi -> mi_idx -> mc CSRs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <fstream>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline std::string_view varlen_get(const uint64_t* off, const char* dat, size_t i) {
    return std::string_view(dat + off[i], off[i+1] - off[i]);
}

static int32_t resolve_dim_id(const std::string& storage,
                              const char* table,
                              const char* text_col,
                              const std::string& literal) {
    std::string base = storage + "/" + table + "/";
    MmapColumn<int32_t> ids(base + "id.bin");
    MmapColumn<uint64_t> off(base + text_col + ".offsets.bin");
    MmapColumn<char>     dat(base + text_col + ".data.bin");
    size_t N = ids.count;
    for (size_t r = 0; r < N; ++r) {
        std::string_view sv(dat.data + off.data[r], off.data[r+1] - off.data[r]);
        if (sv == literal) return ids.data[r];
    }
    return -1;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--params...]\n", argv[0]);
        return 1;
    }
    GENDB_PHASE("total");

    std::string storage = argv[1];
    std::string results_dir = argv[2];

    // Parameters
    int64_t production_year_lower = parse_int_arg(argc, argv, "--production_year_lower", 2005);
    double  info_upper_d          = parse_double_arg(argc, argv, "--info_upper", 8.5);
    std::string info_eq           = parse_string_arg(argc, argv, "--info_eq", "rating");
    std::string country_code_neq  = parse_string_arg(argc, argv, "--country_code_neq", "[us]");
    std::string info_eq_2         = parse_string_arg(argc, argv, "--info_eq_2", "countries");

    // info_upper is a numeric threshold but compared lexicographically against mi_idx.info
    // The values in mi_idx.info are short numeric strings like "8.5", "1.6".
    // Format upper as ASCII without trailing zeros to match input formatting.
    char info_upper_str[64];
    // try to format like default: "8.5"
    // simplest: trim trailing zeros.
    {
        std::snprintf(info_upper_str, sizeof(info_upper_str), "%.6f", info_upper_d);
        // trim trailing zeros and trailing '.'
        char* p = info_upper_str + std::strlen(info_upper_str) - 1;
        while (p > info_upper_str && *p == '0') *p-- = '\0';
        if (p > info_upper_str && *p == '.') *p = '\0';
    }
    std::string_view info_upper_sv(info_upper_str);

    // ------------------------------------------------------------
    // Load
    // ------------------------------------------------------------
    // Use raw mmap variables (kept in scope for the rest of main)
    MmapColumn<int32_t>  t_id, t_kind_id, t_prod_year;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int32_t>  kt_ids;
    MmapColumn<uint64_t> kt_kind_off;
    MmapColumn<char>     kt_kind_dat;

    MmapColumn<int32_t>  it_ids;
    MmapColumn<uint64_t> it_info_off;
    MmapColumn<char>     it_info_dat;

    MmapColumn<int32_t>  kw_ids;
    MmapColumn<uint64_t> kw_kw_off;
    MmapColumn<char>     kw_kw_dat;

    MmapColumn<int32_t>  cn_ids;
    MmapColumn<uint64_t> cn_cc_off, cn_name_off;
    MmapColumn<char>     cn_cc_dat, cn_name_dat;
    MmapColumn<int32_t>  cn_id_pos;

    // facts
    MmapColumn<int32_t>  mk_keyword_id;
    MmapColumn<uint64_t> mk_off;

    MmapColumn<int32_t>  mi_info_type_id;
    MmapColumn<uint64_t> mi_off;
    MmapColumn<uint64_t> mi_info_voff;
    MmapColumn<char>     mi_info_vdat;

    MmapColumn<int32_t>  mii_info_type_id;
    MmapColumn<uint64_t> mii_off;
    MmapColumn<uint64_t> mii_info_voff;
    MmapColumn<char>     mii_info_vdat;

    MmapColumn<int32_t>  mc_company_id;
    MmapColumn<uint64_t> mc_off;

    {
        GENDB_PHASE("data_loading");

        t_id.open(storage + "/title/id.bin");
        t_kind_id.open(storage + "/title/kind_id.bin");
        t_prod_year.open(storage + "/title/production_year.bin");
        t_title_off.open(storage + "/title/title.offsets.bin");
        t_title_dat.open(storage + "/title/title.data.bin");

        kt_ids.open(storage + "/kind_type/id.bin");
        kt_kind_off.open(storage + "/kind_type/kind.offsets.bin");
        kt_kind_dat.open(storage + "/kind_type/kind.data.bin");

        it_ids.open(storage + "/info_type/id.bin");
        it_info_off.open(storage + "/info_type/info.offsets.bin");
        it_info_dat.open(storage + "/info_type/info.data.bin");

        kw_ids.open(storage + "/keyword/id.bin");
        kw_kw_off.open(storage + "/keyword/keyword.offsets.bin");
        kw_kw_dat.open(storage + "/keyword/keyword.data.bin");

        cn_ids.open(storage + "/company_name/id.bin");
        cn_cc_off.open(storage + "/company_name/country_code.offsets.bin");
        cn_cc_dat.open(storage + "/company_name/country_code.data.bin");
        cn_name_off.open(storage + "/company_name/name.offsets.bin");
        cn_name_dat.open(storage + "/company_name/name.data.bin");
        cn_id_pos.open(storage + "/indexes/company_name__id__pos.bin");

        mk_keyword_id.open(storage + "/movie_keyword/keyword_id.bin");
        mk_off.open(storage + "/indexes/movie_keyword__movie_id__offsets.bin");

        mi_info_type_id.open(storage + "/movie_info/info_type_id.bin");
        mi_off.open(storage + "/indexes/movie_info__movie_id__offsets.bin");
        mi_info_voff.open(storage + "/movie_info/info.offsets.bin");
        mi_info_vdat.open(storage + "/movie_info/info.data.bin");

        mii_info_type_id.open(storage + "/movie_info_idx/info_type_id.bin");
        mii_off.open(storage + "/indexes/movie_info_idx__movie_id__offsets.bin");
        mii_info_voff.open(storage + "/movie_info_idx/info.offsets.bin");
        mii_info_vdat.open(storage + "/movie_info_idx/info.data.bin");

        mc_company_id.open(storage + "/movie_companies/company_id.bin");
        mc_off.open(storage + "/indexes/movie_companies__movie_id__offsets.bin");

        // Random-access patterns on the small fact columns we touch
        mk_keyword_id.advise_random();
        mi_info_type_id.advise_random();
        mii_info_type_id.advise_random();
        mc_company_id.advise_random();
        mi_info_voff.advise_random();
        mi_info_vdat.advise_random();
        mii_info_voff.advise_random();
        mii_info_vdat.advise_random();
        cn_id_pos.advise_random();
        cn_name_off.advise_random();
        cn_name_dat.advise_random();
        cn_cc_off.advise_sequential();
        cn_cc_dat.advise_sequential();
        t_kind_id.advise_sequential();
        t_prod_year.advise_sequential();
        t_id.advise_sequential();
    }

    // ------------------------------------------------------------
    // Resolve dimension literals & build small structures
    // ------------------------------------------------------------
    int32_t it1_id = -1, it2_id = -1; // countries, rating
    {
        size_t N = it_ids.count;
        for (size_t r = 0; r < N; ++r) {
            std::string_view sv(it_info_dat.data + it_info_off.data[r],
                                it_info_off.data[r+1] - it_info_off.data[r]);
            if (sv == info_eq_2) it1_id = it_ids.data[r];
            else if (sv == info_eq) it2_id = it_ids.data[r];
        }
    }
    if (it1_id < 0 || it2_id < 0) {
        std::fprintf(stderr, "Could not resolve info_type ids\n");
        return 2;
    }

    // kt_ids: 'movie','episode'
    std::vector<int32_t> kt_id_vec;
    {
        size_t N = kt_ids.count;
        for (size_t r = 0; r < N; ++r) {
            std::string_view sv(kt_kind_dat.data + kt_kind_off.data[r],
                                kt_kind_off.data[r+1] - kt_kind_off.data[r]);
            if (sv == "movie" || sv == "episode") kt_id_vec.push_back(kt_ids.data[r]);
        }
    }
    // build a tiny bitset over max kind id
    int32_t max_kind_id = 0;
    for (int32_t v : kt_id_vec) if (v > max_kind_id) max_kind_id = v;
    std::vector<uint8_t> kt_bitset(max_kind_id + 2, 0);
    for (int32_t v : kt_id_vec) if (v >= 0) kt_bitset[v] = 1;

    // kw_ids: 4 keywords
    static const std::array<std::string_view, 4> kw_literals = {
        std::string_view("murder"), std::string_view("murder-in-title"),
        std::string_view("blood"),  std::string_view("violence")
    };
    std::unordered_set<int32_t> kw_set;
    {
        size_t N = kw_ids.count;
        for (size_t r = 0; r < N; ++r) {
            std::string_view sv(kw_kw_dat.data + kw_kw_off.data[r],
                                kw_kw_off.data[r+1] - kw_kw_off.data[r]);
            for (auto& lit : kw_literals) {
                if (sv == lit) { kw_set.insert(kw_ids.data[r]); break; }
            }
        }
    }

    // mi info string set (10 strings)
    static const std::array<std::string_view, 10> mi_info_literals = {
        std::string_view("Sweden"), std::string_view("Norway"),
        std::string_view("Germany"), std::string_view("Denmark"),
        std::string_view("Swedish"), std::string_view("Danish"),
        std::string_view("Norwegian"), std::string_view("German"),
        std::string_view("USA"), std::string_view("American")
    };
    auto mi_info_match = [&](std::string_view sv) {
        for (auto& lit : mi_info_literals) if (sv == lit) return true;
        return false;
    };

    // cn_notus_bitset: scan company_name once
    size_t N_cn = cn_ids.count;
    int32_t max_company_id = 0;
    // cn_id_pos has size max_id+2
    int32_t cn_pos_count = (int32_t)cn_id_pos.count;
    max_company_id = cn_pos_count - 2;
    std::vector<uint8_t> cn_notus(max_company_id + 2, 0);
    {
        GENDB_PHASE("build_cn_notus");
        for (size_t r = 0; r < N_cn; ++r) {
            std::string_view sv(cn_cc_dat.data + cn_cc_off.data[r],
                                cn_cc_off.data[r+1] - cn_cc_off.data[r]);
            // Standard SQL: NULL != '[us]' is UNKNOWN, so NULL country_codes are excluded.
            // Filter: cn.country_code != '[us]' (we keep only rows where sv is non-NULL and != '[us]')
            if (!sv.empty() && sv != country_code_neq) {
                int32_t id = cn_ids.data[r];
                if (id >= 0 && id <= max_company_id) cn_notus[id] = 1;
            }
        }
    }

    // ------------------------------------------------------------
    // Drive on title (parallel)
    // ------------------------------------------------------------
    size_t N_t = t_id.count;
    // Each CSR offsets file has size = max_movie_id + 2. The max safe v is count - 2.
    // (so accessing off[v+1] is valid.)
    const int32_t max_v_mk  = (int32_t)mk_off.count  - 2;
    const int32_t max_v_mi  = (int32_t)mi_off.count  - 2;
    const int32_t max_v_mii = (int32_t)mii_off.count - 2;
    const int32_t max_v_mc  = (int32_t)mc_off.count  - 2;
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > 12) nthreads = 12;

    struct ThreadResult {
        bool has = false;
        std::string min_cn_name;
        std::string min_mii_info;
        std::string min_t_title;
    };
    std::vector<ThreadResult> results(nthreads);

    auto worker = [&](unsigned tid, size_t lo, size_t hi) {
        ThreadResult& res = results[tid];
        std::string_view best_cn, best_mii, best_t;
        bool have = false;

        for (size_t r = lo; r < hi; ++r) {
            int32_t py = t_prod_year.data[r];
            if (py <= (int32_t)production_year_lower) continue;
            int32_t kid = t_kind_id.data[r];
            if (kid < 0 || kid > max_kind_id || !kt_bitset[kid]) continue;

            int32_t v = t_id.data[r];
            if (v < 0) continue;

            // Probe mk: any keyword in kw_set?
            if (v > max_v_mk) continue;
            uint64_t mk_lo = mk_off.data[v];
            uint64_t mk_hi = mk_off.data[v+1];
            if (mk_lo == mk_hi) continue;
            bool kw_match = false;
            for (uint64_t k = mk_lo; k < mk_hi; ++k) {
                int32_t kid_fk = mk_keyword_id.data[k];
                if (kw_set.find(kid_fk) != kw_set.end()) { kw_match = true; break; }
            }
            if (!kw_match) continue;

            // Probe mi: at least one row with info_type_id == it1_id AND info in mi_info_literals
            if (v > max_v_mi) continue;
            uint64_t mi_lo = mi_off.data[v];
            uint64_t mi_hi = mi_off.data[v+1];
            if (mi_lo == mi_hi) continue;
            bool mi_match = false;
            for (uint64_t k = mi_lo; k < mi_hi; ++k) {
                if (mi_info_type_id.data[k] != it1_id) continue;
                std::string_view sv(mi_info_vdat.data + mi_info_voff.data[k],
                                    mi_info_voff.data[k+1] - mi_info_voff.data[k]);
                if (mi_info_match(sv)) { mi_match = true; break; }
            }
            if (!mi_match) continue;

            // Probe mi_idx: collect best (smallest) info from rows where info_type_id==it2_id AND info<info_upper
            if (v > max_v_mii) continue;
            uint64_t mii_lo = mii_off.data[v];
            uint64_t mii_hi = mii_off.data[v+1];
            if (mii_lo == mii_hi) continue;
            std::string_view local_best_mii;
            bool mii_found = false;
            for (uint64_t k = mii_lo; k < mii_hi; ++k) {
                if (mii_info_type_id.data[k] != it2_id) continue;
                std::string_view sv(mii_info_vdat.data + mii_info_voff.data[k],
                                    mii_info_voff.data[k+1] - mii_info_voff.data[k]);
                if (!(sv < info_upper_sv)) continue;
                if (!mii_found || sv < local_best_mii) { local_best_mii = sv; mii_found = true; }
            }
            if (!mii_found) continue;

            // Probe mc: collect best (smallest) cn.name among rows where cn_notus[mc.company_id]==1
            if (v > max_v_mc) continue;
            uint64_t mc_lo = mc_off.data[v];
            uint64_t mc_hi = mc_off.data[v+1];
            if (mc_lo == mc_hi) continue;
            std::string_view local_best_cn;
            bool cn_found = false;
            for (uint64_t k = mc_lo; k < mc_hi; ++k) {
                int32_t cid = mc_company_id.data[k];
                if (cid < 0 || cid > max_company_id) continue;
                if (!cn_notus[cid]) continue;
                int32_t pos = cn_id_pos.data[cid];
                if (pos < 0) continue;
                std::string_view sv(cn_name_dat.data + cn_name_off.data[pos],
                                    cn_name_off.data[pos+1] - cn_name_off.data[pos]);
                if (!cn_found || sv < local_best_cn) { local_best_cn = sv; cn_found = true; }
            }
            if (!cn_found) continue;

            // tuple confirmed — update mins
            std::string_view t_title(t_title_dat.data + t_title_off.data[r],
                                     t_title_off.data[r+1] - t_title_off.data[r]);
            if (!have) {
                best_cn = local_best_cn;
                best_mii = local_best_mii;
                best_t = t_title;
                have = true;
            } else {
                if (local_best_cn < best_cn) best_cn = local_best_cn;
                if (local_best_mii < best_mii) best_mii = local_best_mii;
                if (t_title < best_t) best_t = t_title;
            }
        }

        if (have) {
            res.has = true;
            res.min_cn_name.assign(best_cn.data(), best_cn.size());
            res.min_mii_info.assign(best_mii.data(), best_mii.size());
            res.min_t_title.assign(best_t.data(), best_t.size());
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        size_t chunk = (N_t + nthreads - 1) / nthreads;
        for (unsigned tid = 0; tid < nthreads; ++tid) {
            size_t lo = tid * chunk;
            size_t hi = std::min(N_t, lo + chunk);
            if (lo >= hi) continue;
            threads.emplace_back(worker, tid, lo, hi);
        }
        for (auto& th : threads) th.join();
    }

    // Reduce
    bool have = false;
    std::string min_cn_name, min_mii_info, min_t_title;
    for (auto& r : results) {
        if (!r.has) continue;
        if (!have) {
            min_cn_name = r.min_cn_name;
            min_mii_info = r.min_mii_info;
            min_t_title = r.min_t_title;
            have = true;
        } else {
            if (r.min_cn_name < min_cn_name) min_cn_name = r.min_cn_name;
            if (r.min_mii_info < min_mii_info) min_mii_info = r.min_mii_info;
            if (r.min_t_title < min_t_title) min_t_title = r.min_t_title;
        }
    }

    // ------------------------------------------------------------
    // Output CSV
    // ------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q22d.csv";
        std::ofstream out(out_path);
        out << "movie_company,rating,western_violent_movie\n";
        if (have) {
            out << min_cn_name << "," << min_mii_info << "," << min_t_title << "\n";
        } else {
            out << ",,\n";
        }
    }

    return 0;
}
