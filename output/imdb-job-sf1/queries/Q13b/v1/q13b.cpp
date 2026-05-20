// Q13b — JOB benchmark
// MIN(cn.name), MIN(miidx.info), MIN(t.title)
//
// Driver: scan title for kind=movie AND title LIKE '%Champion%' OR '%Loser%'.
// Per surviving mid, probe mc/mi/miidx via offsets-only indexes.

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <sys/stat.h>

using namespace gendb;
namespace fs = std::filesystem;

// ---------- helpers ----------
static int16_t resolve_dict_i16(const std::string& dir, const std::string& col,
                                const std::string& target) {
    MmapColumn<uint64_t> off(dir + "/" + col + ".dict.off");
    MmapColumn<char>     dat(dir + "/" + col + ".dict.dat");
    for (size_t i = 0; i + 1 < off.count; i++) {
        size_t s = off[i], e = off[i + 1];
        if (e - s == target.size() &&
            memcmp(dat.data + s, target.data(), target.size()) == 0) {
            // dict encoding: code 0 = NULL, code k references dict entry k-1
            return (int16_t)(i + 1);
        }
    }
    return -1;
}

// resolve a varlen target string in a small dim table; returns id (from id.bin)
static int32_t resolve_varlen_id(const std::string& table_dir,
                                 const std::string& col,
                                 const std::string& target) {
    MmapColumn<uint64_t> off(table_dir + "/" + col + ".off");
    MmapColumn<char>     dat(table_dir + "/" + col + ".dat");
    MmapColumn<int32_t>  id(table_dir + "/id.bin");
    for (size_t i = 0; i + 1 < off.count; i++) {
        size_t s = off[i], e = off[i + 1];
        if (e - s == target.size() &&
            memcmp(dat.data + s, target.data(), target.size()) == 0) {
            return id[i];
        }
    }
    return -1;
}

// memmem-style search for needle in [haystack, haystack+n)
static inline bool contains_bytes(const char* h, size_t n,
                                  const char* needle, size_t m) {
    if (m == 0 || n < m) return false;
    return memmem(h, n, needle, m) != nullptr;
}

// bytewise MIN update
static inline void update_min(std::string& cur, const char* s, size_t n) {
    if (cur.empty()) { cur.assign(s, n); return; }
    int c = memcmp(s, cur.data(), std::min((size_t)cur.size(), n));
    if (c < 0 || (c == 0 && n < cur.size())) cur.assign(s, n);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---------- 1) Resolve dims ----------
    int16_t us_code;
    int32_t ct_id, it_id, it2_id, kt_id;
    {
        GENDB_PHASE("dim_resolve");
        us_code = resolve_dict_i16(gendb_dir + "/company_name", "country_code", "[us]");
        ct_id   = resolve_varlen_id(gendb_dir + "/company_type", "kind", "production companies");
        it_id   = resolve_varlen_id(gendb_dir + "/info_type",    "info", "rating");
        it2_id  = resolve_varlen_id(gendb_dir + "/info_type",    "info", "release dates");
        kt_id   = resolve_varlen_id(gendb_dir + "/kind_type",    "kind", "movie");
        if (us_code < 0 || ct_id < 0 || it_id < 0 || it2_id < 0 || kt_id < 0) {
            std::fprintf(stderr, "dim resolution failed: us=%d ct=%d it=%d it2=%d kt=%d\n",
                         us_code, ct_id, it_id, it2_id, kt_id);
            return 2;
        }
    }

    // ---------- 2) mmap data ----------
    MmapColumn<int32_t>  t_kind_id;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<uint32_t> mc_offsets;
    MmapColumn<int32_t>  mc_company_type_id;
    MmapColumn<int32_t>  mc_company_id;

    MmapColumn<uint32_t> mi_offsets;
    MmapColumn<int32_t>  mi_info_type_id;

    MmapColumn<uint32_t> miidx_offsets;
    MmapColumn<int32_t>  miidx_info_type_id;
    MmapColumn<uint64_t> miidx_info_off;
    MmapColumn<char>     miidx_info_dat;

    MmapColumn<int16_t>  cn_country;
    MmapColumn<uint64_t> cn_name_off;
    MmapColumn<char>     cn_name_dat;

    size_t n_title = 0;
    {
        GENDB_PHASE("data_loading");
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");
        n_title = t_kind_id.count;

        mc_offsets.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");

        mi_offsets.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");

        miidx_offsets.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        cn_country.open(gendb_dir + "/company_name/country_code.bin");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        // Random access patterns on probe columns
        mc_offsets.advise_random();
        mc_company_type_id.advise_random();
        mc_company_id.advise_random();
        mi_offsets.advise_random();
        mi_info_type_id.advise_random();
        miidx_offsets.advise_random();
        miidx_info_type_id.advise_random();
        miidx_info_off.advise_random();
        miidx_info_dat.advise_random();
        cn_country.advise_random();
        cn_name_off.advise_random();
        cn_name_dat.advise_random();
    }

    // ---------- 3) Parallel scan title -> candidate mids ----------
    // Build candidate mids in chunks. Then process candidates.
    const size_t MORSEL = 64 * 1024;
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > 12) nthreads = 12;

    struct ThreadResult {
        std::string min_cn_name;
        std::string min_miidx_info;
        std::string min_title;
    };
    std::vector<ThreadResult> results(nthreads);

    const char* needle1 = "Champion"; const size_t n1 = 8;
    const char* needle2 = "Loser";    const size_t n2 = 5;

    std::atomic<size_t> morsel_cursor{0};

    auto worker = [&](unsigned tid) {
        ThreadResult& R = results[tid];
        std::string& min_cn   = R.min_cn_name;
        std::string& min_info = R.min_miidx_info;
        std::string& min_t    = R.min_title;

        const int32_t  *kind_id   = t_kind_id.data;
        const uint64_t *t_off     = t_title_off.data;
        const char     *t_dat     = t_title_dat.data;

        const uint32_t *mc_off    = mc_offsets.data;
        const int32_t  *mc_cti    = mc_company_type_id.data;
        const int32_t  *mc_cid    = mc_company_id.data;

        const uint32_t *mi_off    = mi_offsets.data;
        const int32_t  *mi_iti    = mi_info_type_id.data;

        const uint32_t *mx_off    = miidx_offsets.data;
        const int32_t  *mx_iti    = miidx_info_type_id.data;
        const uint64_t *mx_info_o = miidx_info_off.data;
        const char     *mx_info_d = miidx_info_dat.data;

        const int16_t  *cn_cc     = cn_country.data;
        const uint64_t *cn_off    = cn_name_off.data;
        const char     *cn_dat    = cn_name_dat.data;

        while (true) {
            size_t start = morsel_cursor.fetch_add(MORSEL, std::memory_order_relaxed);
            if (start >= n_title) break;
            size_t end = std::min(start + MORSEL, n_title);

            for (size_t i = start; i < end; i++) {
                // length prune
                uint64_t lo = t_off[i];
                uint64_t hi = t_off[i + 1];
                uint64_t tlen = hi - lo;
                if (tlen < 5) continue;
                // kind filter
                if (kind_id[i] != kt_id) continue;
                // memmem for either needle
                const char* tp = t_dat + lo;
                bool match = false;
                if (tlen >= n1 && memmem(tp, tlen, needle1, n1)) match = true;
                else if (memmem(tp, tlen, needle2, n2))         match = true;
                if (!match) continue;

                uint32_t mid = (uint32_t)(i + 1);

                // --- movie_info existence check (it2_id = release dates) ---
                uint32_t mi_lo = mi_off[mid];
                uint32_t mi_hi = mi_off[mid + 1];
                bool mi_ok = false;
                for (uint32_t r = mi_lo; r < mi_hi; r++) {
                    if (mi_iti[r] == it2_id) { mi_ok = true; break; }
                }
                if (!mi_ok) continue;

                // --- movie_companies probe (ct + cn[us]) ---
                uint32_t mc_lo = mc_off[mid];
                uint32_t mc_hi = mc_off[mid + 1];
                // collect candidate cn.names from matching rows
                bool mc_any = false;
                std::string best_cn;
                for (uint32_t r = mc_lo; r < mc_hi; r++) {
                    if (mc_cti[r] != ct_id) continue;
                    int32_t cid = mc_cid[r];
                    if (cid <= 0) continue;
                    if (cn_cc[cid - 1] != us_code) continue;
                    // valid cn match
                    uint64_t no = cn_off[cid - 1];
                    uint64_t ne = cn_off[cid];
                    size_t nlen = (size_t)(ne - no);
                    const char* np = cn_dat + no;
                    if (best_cn.empty()) {
                        best_cn.assign(np, nlen);
                    } else {
                        int c = memcmp(np, best_cn.data(), std::min(best_cn.size(), nlen));
                        if (c < 0 || (c == 0 && nlen < best_cn.size())) best_cn.assign(np, nlen);
                    }
                    mc_any = true;
                }
                if (!mc_any) continue;

                // --- movie_info_idx probe (it_id = rating) ---
                uint32_t mx_lo = mx_off[mid];
                uint32_t mx_hi = mx_off[mid + 1];
                bool mx_any = false;
                std::string best_info;
                for (uint32_t r = mx_lo; r < mx_hi; r++) {
                    if (mx_iti[r] != it_id) continue;
                    uint64_t io = mx_info_o[r];
                    uint64_t ie = mx_info_o[r + 1];
                    size_t ilen = (size_t)(ie - io);
                    const char* ip = mx_info_d + io;
                    if (best_info.empty()) {
                        best_info.assign(ip, ilen);
                    } else {
                        int c = memcmp(ip, best_info.data(), std::min(best_info.size(), ilen));
                        if (c < 0 || (c == 0 && ilen < best_info.size())) best_info.assign(ip, ilen);
                    }
                    mx_any = true;
                }
                if (!mx_any) continue;

                // All joins satisfied — update mins
                update_min(min_cn,   best_cn.data(),   best_cn.size());
                update_min(min_info, best_info.data(), best_info.size());
                update_min(min_t,    tp,               (size_t)tlen);
            }
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> ts;
        ts.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; t++) ts.emplace_back(worker, t);
        for (auto& th : ts) th.join();
    }

    // ---------- 4) Merge ----------
    std::string min_cn, min_info, min_t;
    for (auto& R : results) {
        if (!R.min_cn_name.empty())
            update_min(min_cn, R.min_cn_name.data(), R.min_cn_name.size());
        if (!R.min_miidx_info.empty())
            update_min(min_info, R.min_miidx_info.data(), R.min_miidx_info.size());
        if (!R.min_title.empty())
            update_min(min_t, R.min_title.data(), R.min_title.size());
    }

    // ---------- 5) Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q13b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "producing_company,rating,movie_about_winning\n");
        std::fprintf(f, "%s,%s,%s\n",
                     min_cn.c_str(), min_info.c_str(), min_t.c_str());
        std::fclose(f);
    }

    return 0;
}
