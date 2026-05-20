// Q13c — MIN(cn.name), MIN(miidx.info), MIN(t.title) with anchored prefix LIKE
// Driver: tiny title candidate set (~hundreds) -> index nested loops via offset arrays.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;
namespace fs = std::filesystem;

// Find dict code (1-based) for an exact value in a varlen dict (.dict.off int64, .dict.dat).
// Returns 0 if not found.
static int32_t resolve_dict_code(const std::string& dir,
                                 const std::string& col,
                                 const std::string& target) {
    MmapColumn<int64_t> off(dir + "/" + col + ".dict.off");
    MmapColumn<char>   dat(dir + "/" + col + ".dict.dat");
    size_t K = off.count > 0 ? off.count - 1 : 0;
    for (size_t i = 0; i < K; i++) {
        int64_t lo = off[i], hi = off[i + 1];
        size_t len = (size_t)(hi - lo);
        if (len == target.size() && std::memcmp(dat.data + lo, target.data(), len) == 0) {
            return (int32_t)(i + 1);
        }
    }
    return 0;
}

// Resolve scalar id for a small dim table: look up row where varlen col == target,
// then return id.bin[row].
static int32_t resolve_dim_id(const std::string& dir,
                              const std::string& col,
                              const std::string& target) {
    MmapColumn<int64_t> off(dir + "/" + col + ".off");
    MmapColumn<char>   dat(dir + "/" + col + ".dat");
    MmapColumn<int32_t> id(dir + "/id.bin");
    size_t N = id.count;
    for (size_t i = 0; i < N; i++) {
        int64_t lo = off[i], hi = off[i + 1];
        size_t len = (size_t)(hi - lo);
        if (len == target.size() && std::memcmp(dat.data + lo, target.data(), len) == 0) {
            return id[i];
        }
    }
    return -1;
}

// Bytewise min update of std::string (mimics MIN over UTF-8 strings).
static inline void min_update(std::string& cur, const char* p, size_t n) {
    if (n == 0) return;
    if (cur.empty()) { cur.assign(p, n); return; }
    int c = std::memcmp(cur.data(), p, std::min(cur.size(), n));
    if (c < 0) return;
    if (c > 0) { cur.assign(p, n); return; }
    // common prefix matches — shorter wins
    if (n < cur.size()) cur.assign(p, n);
}

static inline void min_merge(std::string& a, const std::string& b) {
    if (b.empty()) return;
    min_update(a, b.data(), b.size());
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // -------------------- dim resolution --------------------
    int32_t us_code, ct_id, it_id, it2_id, kt_id;
    {
        GENDB_PHASE("dim_resolution");
        us_code = resolve_dict_code(gendb_dir + "/company_name", "country_code", "[us]");
        ct_id   = resolve_dim_id(gendb_dir + "/company_type", "kind", "production companies");
        it_id   = resolve_dim_id(gendb_dir + "/info_type",   "info", "rating");
        it2_id  = resolve_dim_id(gendb_dir + "/info_type",   "info", "release dates");
        kt_id   = resolve_dim_id(gendb_dir + "/kind_type",   "kind", "movie");
        if (us_code == 0 || ct_id < 0 || it_id < 0 || it2_id < 0 || kt_id < 0) {
            std::fprintf(stderr, "dim resolution failed: us=%d ct=%d it=%d it2=%d kt=%d\n",
                         us_code, ct_id, it_id, it2_id, kt_id);
            return 2;
        }
    }

    // -------------------- mmap columns --------------------
    MmapColumn<int32_t> t_kind_id, t_id;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mc_off_idx;
    MmapColumn<int32_t> mc_company_type_id, mc_company_id;

    MmapColumn<int32_t> mi_off_idx;
    MmapColumn<int32_t> mi_info_type_id;

    MmapColumn<int32_t> miidx_off_idx;
    MmapColumn<int32_t> miidx_info_type_id;
    MmapColumn<int64_t> miidx_info_off;
    MmapColumn<char>    miidx_info_dat;

    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_name_off;
    MmapColumn<char>    cn_name_dat;

    {
        GENDB_PHASE("data_loading");
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_id.open(gendb_dir + "/title/id.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mc_off_idx.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");

        mi_off_idx.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");

        miidx_off_idx.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        cn_cc.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        // random access patterns on probes
        mc_off_idx.advise_random();
        mc_company_type_id.advise_random();
        mc_company_id.advise_random();
        mi_off_idx.advise_random();
        mi_info_type_id.advise_random();
        miidx_off_idx.advise_random();
        miidx_info_type_id.advise_random();
        miidx_info_off.advise_random();
        miidx_info_dat.advise_random();
        cn_cc.advise_random();
        cn_name_off.advise_random();
        cn_name_dat.advise_random();
    }

    // -------------------- scan title (parallel) --------------------
    static const char  PFX1[] = "Champion"; static const size_t PFX1_LEN = 8;
    static const char  PFX2[] = "Loser";    static const size_t PFX2_LEN = 5;

    size_t N = t_kind_id.count;
    std::vector<int32_t> mids;
    {
        GENDB_PHASE("main_scan");
        int nthreads = std::max(1, omp_get_max_threads());
        std::vector<std::vector<int32_t>> per_thread(nthreads);
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& v = per_thread[tid];
            #pragma omp for schedule(static)
            for (size_t i = 0; i < N; i++) {
                if (t_kind_id[i] != kt_id) continue;
                int64_t lo = t_title_off[i];
                int64_t hi = t_title_off[i + 1];
                size_t len = (size_t)(hi - lo);
                if (len == 0) continue;
                const char* p = t_title_dat.data + lo;
                bool match = false;
                if (len >= PFX1_LEN && std::memcmp(p, PFX1, PFX1_LEN) == 0) match = true;
                else if (len >= PFX2_LEN && std::memcmp(p, PFX2, PFX2_LEN) == 0) match = true;
                if (!match) continue;
                v.push_back(t_id[i]);
            }
        }
        size_t total = 0;
        for (auto& v : per_thread) total += v.size();
        mids.reserve(total);
        for (auto& v : per_thread) mids.insert(mids.end(), v.begin(), v.end());
    }

    // -------------------- probe + aggregate --------------------
    std::string min_cn_name, min_info, min_title;
    {
        GENDB_PHASE("probe_and_aggregate");
        int nthreads = std::max(1, omp_get_max_threads());
        std::vector<std::string> tcn(nthreads), tinfo(nthreads), ttitle(nthreads);

        size_t M = mids.size();
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string& mcn   = tcn[tid];
            std::string& minf  = tinfo[tid];
            std::string& mtit  = ttitle[tid];

            #pragma omp for schedule(dynamic, 16)
            for (size_t k = 0; k < M; k++) {
                int32_t mid = mids[k];

                // movie_info existence with it2_id
                int32_t mi_lo = mi_off_idx[mid];
                int32_t mi_hi = mi_off_idx[mid + 1];
                bool mi_ok = false;
                for (int32_t r = mi_lo; r < mi_hi; r++) {
                    if (mi_info_type_id[r] == it2_id) { mi_ok = true; break; }
                }
                if (!mi_ok) continue;

                // movie_companies: collect qualifying cn.names (local min)
                int32_t mc_lo = mc_off_idx[mid];
                int32_t mc_hi = mc_off_idx[mid + 1];
                std::string local_cn;
                bool has_cn = false;
                for (int32_t r = mc_lo; r < mc_hi; r++) {
                    if (mc_company_type_id[r] != ct_id) continue;
                    int32_t cid = mc_company_id[r];
                    if (cn_cc[cid - 1] != (int16_t)us_code) continue;
                    int64_t no_lo = cn_name_off[cid - 1];
                    int64_t no_hi = cn_name_off[cid];
                    const char* np = cn_name_dat.data + no_lo;
                    size_t nlen = (size_t)(no_hi - no_lo);
                    if (!has_cn) { local_cn.assign(np, nlen); has_cn = true; }
                    else { min_update(local_cn, np, nlen); }
                }
                if (!has_cn) continue;

                // movie_info_idx: filter info_type_id == it_id, capture info
                int32_t mx_lo = miidx_off_idx[mid];
                int32_t mx_hi = miidx_off_idx[mid + 1];
                std::string local_info;
                bool has_info = false;
                for (int32_t r = mx_lo; r < mx_hi; r++) {
                    if (miidx_info_type_id[r] != it_id) continue;
                    int64_t io_lo = miidx_info_off[r];
                    int64_t io_hi = miidx_info_off[r + 1];
                    const char* ip = miidx_info_dat.data + io_lo;
                    size_t ilen = (size_t)(io_hi - io_lo);
                    if (!has_info) { local_info.assign(ip, ilen); has_info = true; }
                    else { min_update(local_info, ip, ilen); }
                }
                if (!has_info) continue;

                // title qualifies — read t.title and update mins
                // need title row index = mid - 1 (dense PK 1..N)
                size_t ti = (size_t)(mid - 1);
                int64_t to_lo = t_title_off[ti];
                int64_t to_hi = t_title_off[ti + 1];
                const char* tp = t_title_dat.data + to_lo;
                size_t tlen = (size_t)(to_hi - to_lo);

                min_merge(mcn, local_cn);
                min_merge(minf, local_info);
                min_update(mtit, tp, tlen);
            }
        }

        for (int t = 0; t < nthreads; t++) {
            min_merge(min_cn_name, tcn[t]);
            min_merge(min_info, tinfo[t]);
            min_merge(min_title, ttitle[t]);
        }
    }

    // -------------------- output --------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q13c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "producing_company,rating,movie_about_winning\n");
        std::fwrite(min_cn_name.data(), 1, min_cn_name.size(), f);
        std::fputc(',', f);
        std::fwrite(min_info.data(), 1, min_info.size(), f);
        std::fputc(',', f);
        std::fwrite(min_title.data(), 1, min_title.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    return 0;
}
