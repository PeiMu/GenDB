// Q13a — MIN(mi.info), MIN(miidx.info), MIN(t.title)
// Driver: company_name (country_code=='[de]') -> mc CSR -> filter ct -> title kind=movie
//         -> probe mi (release dates) -> probe miidx (rating) -> MIN aggregation.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

struct VarlenRef {
    const char* ptr;  // nullptr means "no value yet" (treated as +inf)
    uint32_t len;
};

static inline bool varlen_lt(const VarlenRef& a, const VarlenRef& b) {
    // Returns true if a < b lexicographically.
    if (a.ptr == nullptr) return false;
    if (b.ptr == nullptr) return true;
    uint32_t m = a.len < b.len ? a.len : b.len;
    int c = std::memcmp(a.ptr, b.ptr, m);
    if (c != 0) return c < 0;
    return a.len < b.len;
}

// Scan a varlen .off/.dat column to find rows matching `target`.
// Returns 1-based id (since identity tables have id == row+1).
static int32_t find_varlen_id(const int64_t* off, size_t off_count, const char* dat,
                              const std::string& target) {
    size_t N = off_count - 1;
    size_t tlen = target.size();
    for (size_t i = 0; i < N; i++) {
        uint64_t s = (uint64_t)off[i];
        uint64_t e = (uint64_t)off[i + 1];
        size_t len = e - s;
        if (len == tlen && std::memcmp(dat + s, target.data(), tlen) == 0) {
            return static_cast<int32_t>(i + 1);
        }
    }
    return -1;
}

// Resolve a dict-encoded string to its dict code (0-based index).
static int32_t resolve_dict_code(const int64_t* dict_off, size_t off_count,
                                 const char* dict_dat, const std::string& target) {
    size_t N = off_count - 1;
    size_t tlen = target.size();
    for (size_t i = 0; i < N; i++) {
        uint64_t s = (uint64_t)dict_off[i];
        uint64_t e = (uint64_t)dict_off[i + 1];
        size_t len = e - s;
        if (len == tlen && std::memcmp(dict_dat + s, target.data(), tlen) == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // ---------- Mmap all needed columns + indexes ----------
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_cc_dict_off;
    MmapColumn<char>    cn_cc_dict_dat;

    MmapColumn<int64_t> ct_kind_off;
    MmapColumn<char>    ct_kind_dat;

    MmapColumn<int64_t> it_info_off;
    MmapColumn<char>    it_info_dat;

    MmapColumn<int64_t> kt_kind_off;
    MmapColumn<char>    kt_kind_dat;

    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_type_id;

    MmapColumn<int32_t> mc_cid_off;
    MmapColumn<int32_t> mc_cid_rowids;

    MmapColumn<int32_t> mi_movie_id_offsets;
    MmapColumn<int32_t> miidx_movie_id_offsets;

    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;

    MmapColumn<int32_t> miidx_info_type_id;
    MmapColumn<int64_t> miidx_info_off;
    MmapColumn<char>    miidx_info_dat;

    MmapColumn<int32_t> t_kind_id;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(gendb_dir + "/company_name/country_code.bin");
        cn_cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");

        ct_kind_off.open(gendb_dir + "/company_type/kind.off");
        ct_kind_dat.open(gendb_dir + "/company_type/kind.dat");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        kt_kind_off.open(gendb_dir + "/kind_type/kind.off");
        kt_kind_dat.open(gendb_dir + "/kind_type/kind.dat");

        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");

        mc_cid_off.open(gendb_dir + "/_idx/movie_companies__company_id__offsets.bin");
        mc_cid_rowids.open(gendb_dir + "/_idx/movie_companies__company_id__rowids.bin");

        mi_movie_id_offsets.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        miidx_movie_id_offsets.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");

        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        // Prefetch larger columns
        mmap_prefetch_all(mc_movie_id, mc_company_type_id, mc_cid_rowids,
                          mi_info_type_id, miidx_info_type_id,
                          mi_movie_id_offsets, miidx_movie_id_offsets);
    }

    // ---------- Resolve dim ids ----------
    // The country_code.bin stores (dict_idx + 1); 0 is reserved for NULL.
    int32_t de_dict_idx = resolve_dict_code(cn_cc_dict_off.data, cn_cc_dict_off.count,
                                            cn_cc_dict_dat.data, "[de]");
    int32_t de_code = (de_dict_idx < 0) ? -1 : (de_dict_idx + 1);
    int32_t ct_id = find_varlen_id(ct_kind_off.data, ct_kind_off.count,
                                   ct_kind_dat.data, "production companies");
    int32_t it_id = find_varlen_id(it_info_off.data, it_info_off.count,
                                   it_info_dat.data, "rating");
    int32_t it2_id = find_varlen_id(it_info_off.data, it_info_off.count,
                                    it_info_dat.data, "release dates");
    int32_t kt_id = find_varlen_id(kt_kind_off.data, kt_kind_off.count,
                                   kt_kind_dat.data, "movie");

    if (de_code < 0 || ct_id < 0 || it_id < 0 || it2_id < 0 || kt_id < 0) {
        std::fprintf(stderr, "Dim resolution failed: de_code=%d ct_id=%d it_id=%d it2_id=%d kt_id=%d\n",
                     de_code, ct_id, it_id, it2_id, kt_id);
        return 2;
    }

    // ---------- Scan company_name.country_code → cn_de list ----------
    std::vector<int32_t> cn_de;
    cn_de.reserve(4096);
    {
        GENDB_PHASE("dim_filter");
        const int16_t de = static_cast<int16_t>(de_code);
        const int16_t* cc = cn_cc.data;
        size_t N = cn_cc.count;
        for (size_t i = 0; i < N; i++) {
            if (cc[i] == de) cn_de.push_back(static_cast<int32_t>(i + 1));  // 1-based id
        }
    }

    // ---------- Driver loop: cn_de → mc CSR → filter ct → title kind → probe miidx + mi ----------
    // Thread-local MIN accumulators.
    VarlenRef global_min_mi   {nullptr, 0};  // release_date
    VarlenRef global_min_midx {nullptr, 0};  // rating
    VarlenRef global_min_tt   {nullptr, 0};  // german_movie

    const int32_t* mc_off_p     = mc_cid_off.data;
    const int32_t* mc_rowids_p  = mc_cid_rowids.data;
    const int32_t* mc_mid_p     = mc_movie_id.data;
    const int32_t* mc_ct_p      = mc_company_type_id.data;
    const int32_t* t_kind_p     = t_kind_id.data;
    const int64_t* t_title_off_p = t_title_off.data;
    const char*    t_title_dat_p = t_title_dat.data;

    const int32_t* mi_off_p      = mi_movie_id_offsets.data;
    const int32_t* mi_itid_p     = mi_info_type_id.data;
    const int64_t* mi_info_off_p = mi_info_off.data;
    const char*    mi_info_dat_p = mi_info_dat.data;

    const int32_t* midx_off_p      = miidx_movie_id_offsets.data;
    const int32_t* midx_itid_p     = miidx_info_type_id.data;
    const int64_t* midx_info_off_p = miidx_info_off.data;
    const char*    midx_info_dat_p = miidx_info_dat.data;

    {
        GENDB_PHASE("main_scan");
        int nth = omp_get_max_threads();
        std::vector<VarlenRef> tl_min_mi(nth,   {nullptr, 0});
        std::vector<VarlenRef> tl_min_midx(nth, {nullptr, 0});
        std::vector<VarlenRef> tl_min_tt(nth,   {nullptr, 0});

        size_t Ncn = cn_de.size();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            VarlenRef lm_mi   = {nullptr, 0};
            VarlenRef lm_midx = {nullptr, 0};
            VarlenRef lm_tt   = {nullptr, 0};

            #pragma omp for schedule(dynamic, 64) nowait
            for (size_t i = 0; i < Ncn; i++) {
                int32_t cn_id = cn_de[i];
                int32_t mc_lo = mc_off_p[cn_id];
                int32_t mc_hi = mc_off_p[cn_id + 1];

                for (int32_t k = mc_lo; k < mc_hi; k++) {
                    int32_t mc_row = mc_rowids_p[k];
                    if (mc_ct_p[mc_row] != ct_id) continue;

                    int32_t mid = mc_mid_p[mc_row];
                    if (mid <= 0) continue;
                    if (t_kind_p[mid - 1] != kt_id) continue;

                    // Probe miidx for rating
                    int32_t midx_lo = midx_off_p[mid];
                    int32_t midx_hi = midx_off_p[mid + 1];
                    VarlenRef best_midx_for_mid = {nullptr, 0};
                    for (int32_t r = midx_lo; r < midx_hi; r++) {
                        if (midx_itid_p[r] != it_id) continue;
                        uint64_t s = (uint64_t)midx_info_off_p[r];
                        uint64_t e = (uint64_t)midx_info_off_p[r + 1];
                        VarlenRef v { midx_info_dat_p + s, (uint32_t)(e - s) };
                        if (varlen_lt(v, best_midx_for_mid)) best_midx_for_mid = v;
                    }
                    if (best_midx_for_mid.ptr == nullptr) continue;  // no rating row

                    // Probe mi for release date
                    int32_t mi_lo = mi_off_p[mid];
                    int32_t mi_hi = mi_off_p[mid + 1];
                    VarlenRef best_mi_for_mid = {nullptr, 0};
                    for (int32_t r = mi_lo; r < mi_hi; r++) {
                        if (mi_itid_p[r] != it2_id) continue;
                        uint64_t s = (uint64_t)mi_info_off_p[r];
                        uint64_t e = (uint64_t)mi_info_off_p[r + 1];
                        VarlenRef v { mi_info_dat_p + s, (uint32_t)(e - s) };
                        if (varlen_lt(v, best_mi_for_mid)) best_mi_for_mid = v;
                    }
                    if (best_mi_for_mid.ptr == nullptr) continue;  // no release date row

                    // Title for this mid
                    uint64_t ts = (uint64_t)t_title_off_p[mid - 1];
                    uint64_t te = (uint64_t)t_title_off_p[mid];
                    VarlenRef vt { t_title_dat_p + ts, (uint32_t)(te - ts) };

                    if (varlen_lt(best_midx_for_mid, lm_midx)) lm_midx = best_midx_for_mid;
                    if (varlen_lt(best_mi_for_mid,   lm_mi))   lm_mi   = best_mi_for_mid;
                    if (varlen_lt(vt, lm_tt))                  lm_tt   = vt;
                }
            }

            tl_min_mi[tid]   = lm_mi;
            tl_min_midx[tid] = lm_midx;
            tl_min_tt[tid]   = lm_tt;
        }

        for (int t = 0; t < nth; t++) {
            if (varlen_lt(tl_min_mi[t],   global_min_mi))   global_min_mi   = tl_min_mi[t];
            if (varlen_lt(tl_min_midx[t], global_min_midx)) global_min_midx = tl_min_midx[t];
            if (varlen_lt(tl_min_tt[t],   global_min_tt))   global_min_tt   = tl_min_tt[t];
        }
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q13a.csv";
        std::ofstream out(out_path);
        out << "release_date,rating,german_movie\n";
        auto write_field = [&](const VarlenRef& v) {
            if (v.ptr == nullptr) return;
            out.write(v.ptr, v.len);
        };
        write_field(global_min_mi);   out << ',';
        write_field(global_min_midx); out << ',';
        write_field(global_min_tt);   out << '\n';
    }

    return 0;
}
