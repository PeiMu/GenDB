// Q13d - generated implementation
// SELECT MIN(cn.name), MIN(miidx.info), MIN(t.title)
// Driver: company_name filtered by country_code='[us]', expand via mc CSR by company_id.

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sys/stat.h>

using namespace gendb;
using std::string;
using std::string_view;

namespace fs = std::filesystem;

// ---------- helpers ----------
static int32_t find_small_varlen_id(const string& off_path, const string& dat_path,
                                     const char* target) {
    MmapColumn<uint64_t> off(off_path);
    MmapColumn<char>     dat(dat_path);
    size_t n = off.count > 0 ? off.count - 1 : 0;
    size_t tlen = std::strlen(target);
    for (size_t i = 0; i < n; ++i) {
        uint64_t lo = off[i], hi = off[i+1];
        size_t len = hi - lo;
        if (len == tlen && std::memcmp(dat.data + lo, target, tlen) == 0) {
            return static_cast<int32_t>(i + 1); // dense PK starting at 1
        }
    }
    return -1;
}

static int16_t find_dict_code(const string& off_path, const string& dat_path,
                              const char* target) {
    MmapColumn<uint64_t> off(off_path);
    MmapColumn<char>     dat(dat_path);
    size_t n = off.count > 0 ? off.count - 1 : 0;
    size_t tlen = std::strlen(target);
    for (size_t i = 0; i < n; ++i) {
        uint64_t lo = off[i], hi = off[i+1];
        size_t len = hi - lo;
        if (len == tlen && std::memcmp(dat.data + lo, target, tlen) == 0) {
            // Dict slot i maps to code (i+1); code 0 reserved for NULL/empty
            return static_cast<int16_t>(i + 1);
        }
    }
    return -1;
}

static inline string_view varlen_get(const uint64_t* off, const char* dat, size_t idx) {
    uint64_t lo = off[idx], hi = off[idx+1];
    return string_view(dat + lo, hi - lo);
}

static void csv_write_field(std::ostream& os, string_view s) {
    bool need_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        os.write(s.data(), s.size());
    } else {
        os.put('"');
        for (char c : s) {
            if (c == '"') os.put('"');
            os.put(c);
        }
        os.put('"');
    }
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const string gendb_dir   = argv[1];
    const string results_dir = argv[2];
    fs::create_directories(results_dir);

    // ---------- dim resolution + data load ----------
    int16_t us_code;
    int32_t ct_id, it_id, it2_id, kt_id;

    // mmaps for main scan
    MmapColumn<int16_t>  cn_cc;
    MmapColumn<uint64_t> cn_name_off;
    MmapColumn<char>     cn_name_dat;

    MmapColumn<int32_t>  mc_cid_off;     // CSR offsets by company_id
    MmapColumn<int32_t>  mc_cid_rowids;  // CSR rowids
    MmapColumn<int32_t>  mc_movie_id;
    MmapColumn<int32_t>  mc_ctid;        // company_type_id

    MmapColumn<int32_t>  t_kind_id;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int32_t>  mi_off;         // CSR offsets only, by movie_id
    MmapColumn<int32_t>  mi_itid;        // info_type_id values

    MmapColumn<int32_t>  miidx_off;      // CSR offsets only, by movie_id
    MmapColumn<int32_t>  miidx_itid;     // info_type_id values
    MmapColumn<uint64_t> miidx_info_off;
    MmapColumn<char>     miidx_info_dat;

    {
        GENDB_PHASE("data_loading");

        us_code = find_dict_code(gendb_dir + "/company_name/country_code.dict.off",
                                 gendb_dir + "/company_name/country_code.dict.dat",
                                 "[us]");
        ct_id   = find_small_varlen_id(gendb_dir + "/company_type/kind.off",
                                       gendb_dir + "/company_type/kind.dat",
                                       "production companies");
        it_id   = find_small_varlen_id(gendb_dir + "/info_type/info.off",
                                       gendb_dir + "/info_type/info.dat",
                                       "rating");
        it2_id  = find_small_varlen_id(gendb_dir + "/info_type/info.off",
                                       gendb_dir + "/info_type/info.dat",
                                       "release dates");
        kt_id   = find_small_varlen_id(gendb_dir + "/kind_type/kind.off",
                                       gendb_dir + "/kind_type/kind.dat",
                                       "movie");

        cn_cc.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        mc_cid_off.open(gendb_dir + "/_idx/movie_companies__company_id__offsets.bin");
        mc_cid_rowids.open(gendb_dir + "/_idx/movie_companies__company_id__rowids.bin");
        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_ctid.open(gendb_dir + "/movie_companies/company_type_id.bin");

        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_itid.open(gendb_dir + "/movie_info/info_type_id.bin");

        miidx_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        miidx_itid.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        // Random access on movie_companies columns (mc_row is indexed via CSR; not sequential)
        mc_movie_id.advise_random();
        mc_ctid.advise_random();
        t_kind_id.advise_random();
        mi_off.advise_random();
        miidx_off.advise_random();
        mi_itid.advise_random();
        miidx_itid.advise_random();
    }

    if (us_code < 0 || ct_id < 0 || it_id < 0 || it2_id < 0 || kt_id < 0) {
        std::fprintf(stderr, "dim resolution failed: us=%d ct=%d it=%d it2=%d kt=%d\n",
                     (int)us_code, ct_id, it_id, it2_id, kt_id);
        return 2;
    }

    // ---------- collect cn_us ----------
    std::vector<int32_t> cn_us;
    {
        GENDB_PHASE("cn_filter");
        cn_us.reserve(cn_cc.count / 5);
        size_t n = cn_cc.count;
        const int16_t* cc = cn_cc.data;
        const int16_t target = us_code;
        for (size_t i = 0; i < n; ++i) {
            if (cc[i] == target) {
                cn_us.push_back(static_cast<int32_t>(i + 1)); // 1-based company_name id
            }
        }
    }

    // ---------- parallel main scan ----------
    string_view best_cn, best_info, best_title;
    bool have_cn=false, have_info=false, have_title=false;

    {
        GENDB_PHASE("main_scan");

        unsigned nthreads = std::thread::hardware_concurrency();
        if (nthreads == 0) nthreads = 4;
        if (nthreads > 12) nthreads = 12;
        size_t total = cn_us.size();
        if (total == 0) nthreads = 1;

        struct LocalBest {
            string_view cn;
            string_view info;
            string_view title;
            bool have_cn=false, have_info=false, have_title=false;
        };
        std::vector<LocalBest> locals(nthreads);

        auto worker = [&](unsigned tid) {
            LocalBest& lb = locals[tid];
            size_t lo = (total * tid) / nthreads;
            size_t hi = (total * (tid + 1)) / nthreads;

            const int32_t* mc_off_p   = mc_cid_off.data;
            const int32_t* mc_rids    = mc_cid_rowids.data;
            const int32_t* mc_mid     = mc_movie_id.data;
            const int32_t* mc_ct      = mc_ctid.data;
            const int32_t* tk         = t_kind_id.data;
            const uint64_t* tt_off    = t_title_off.data;
            const char*    tt_dat     = t_title_dat.data;
            const uint64_t* cn_n_off  = cn_name_off.data;
            const char*    cn_n_dat   = cn_name_dat.data;
            const int32_t* mi_o       = mi_off.data;
            const int32_t* mi_t       = mi_itid.data;
            const int32_t* mix_o      = miidx_off.data;
            const int32_t* mix_t      = miidx_itid.data;
            const uint64_t* mix_io    = miidx_info_off.data;
            const char*    mix_id_dat = miidx_info_dat.data;

            const int32_t CT  = ct_id;
            const int32_t IT  = it_id;
            const int32_t IT2 = it2_id;
            const int32_t KT  = kt_id;

            for (size_t i = lo; i < hi; ++i) {
                int32_t cn_id = cn_us[i];
                int32_t mc_lo = mc_off_p[cn_id];
                int32_t mc_hi = mc_off_p[cn_id + 1];
                // cn.name candidate (only matters if any qualifying tuple exists)
                bool any_for_cn = false;
                for (int32_t k = mc_lo; k < mc_hi; ++k) {
                    int32_t mc_row = mc_rids[k];
                    if (mc_ct[mc_row] != CT) continue;
                    int32_t mid = mc_mid[mc_row];
                    if (mid <= 0) continue;
                    if (tk[mid - 1] != KT) continue;

                    // mi existence probe (offsets indexed by movie_id directly)
                    int32_t mi_l = mi_o[mid];
                    int32_t mi_h = mi_o[mid + 1];
                    bool mi_found = false;
                    for (int32_t j = mi_l; j < mi_h; ++j) {
                        if (mi_t[j] == IT2) { mi_found = true; break; }
                    }
                    if (!mi_found) continue;

                    // miidx filter+capture (offsets indexed by movie_id directly)
                    int32_t mx_l = mix_o[mid];
                    int32_t mx_h = mix_o[mid + 1];
                    bool any_miidx = false;
                    for (int32_t j = mx_l; j < mx_h; ++j) {
                        if (mix_t[j] != IT) continue;
                        any_miidx = true;
                        // miidx.info value for this row
                        uint64_t a = mix_io[j];
                        uint64_t b = mix_io[j+1];
                        string_view v(mix_id_dat + a, b - a);
                        if (!lb.have_info || v < lb.info) { lb.info = v; lb.have_info = true; }
                    }
                    if (!any_miidx) continue;

                    // title candidate
                    uint64_t ta = tt_off[mid - 1];
                    uint64_t tb = tt_off[mid];
                    string_view tv(tt_dat + ta, tb - ta);
                    if (!lb.have_title || tv < lb.title) { lb.title = tv; lb.have_title = true; }

                    any_for_cn = true;
                }
                if (any_for_cn) {
                    uint64_t ca = cn_n_off[cn_id - 1];
                    uint64_t cb = cn_n_off[cn_id];
                    string_view cv(cn_n_dat + ca, cb - ca);
                    if (!lb.have_cn || cv < lb.cn) { lb.cn = cv; lb.have_cn = true; }
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            threads.emplace_back(worker, t);
        }
        for (auto& th : threads) th.join();

        for (auto& lb : locals) {
            if (lb.have_cn   && (!have_cn   || lb.cn    < best_cn))    { best_cn = lb.cn; have_cn = true; }
            if (lb.have_info && (!have_info || lb.info  < best_info))  { best_info = lb.info; have_info = true; }
            if (lb.have_title&& (!have_title|| lb.title < best_title)) { best_title = lb.title; have_title = true; }
        }
    }

    // ---------- output ----------
    {
        GENDB_PHASE("output");
        std::ofstream out(results_dir + "/Q13d.csv");
        out << "producing_company,rating,movie\n";
        if (have_cn) csv_write_field(out, best_cn);
        out.put(',');
        if (have_info) csv_write_field(out, best_info);
        out.put(',');
        if (have_title) csv_write_field(out, best_title);
        out.put('\n');
    }

    return 0;
}
