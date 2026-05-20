// Q14b — title-driven star join with murder-themed title LIKE and rating > 6.0
#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "timing_utils.h"

static void* map_file(const std::string& path, size_t* out_sz) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st; fstat(fd, &st);
    size_t sz = (size_t)st.st_size;
    void* p = (sz == 0) ? nullptr : mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (sz > 0 && p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    close(fd);
    if (out_sz) *out_sz = sz;
    return p;
}

template<typename T>
static const T* map_as(const std::string& path, size_t* out_count) {
    size_t sz = 0;
    void* p = map_file(path, &sz);
    if (out_count) *out_count = sz / sizeof(T);
    return reinterpret_cast<const T*>(p);
}

static inline bool lex_gt_60(const char* a, size_t n) {
    // returns a > "6.0"
    static const char* B = "6.0";
    size_t bn = 3;
    size_t k = n < bn ? n : bn;
    int c = std::memcmp(a, B, k);
    if (c != 0) return c > 0;
    return n > bn;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) { std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]); return 1; }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    // ------------- Load data -------------
    // info_type
    size_t it_off_n = 0, it_dat_n = 0;
    const int64_t* it_off = map_as<int64_t>(store + "/info_type/info.off", &it_off_n);
    const char*    it_dat = (const char*)map_file(store + "/info_type/info.dat", &it_dat_n);
    // kind_type
    size_t kt_off_n = 0, kt_dat_n = 0;
    const int64_t* kt_off = map_as<int64_t>(store + "/kind_type/kind.off", &kt_off_n);
    const char*    kt_dat = (const char*)map_file(store + "/kind_type/kind.dat", &kt_dat_n);
    // keyword
    size_t kw_off_n = 0, kw_dat_n = 0;
    const int64_t* kw_off = map_as<int64_t>(store + "/keyword/keyword.off", &kw_off_n);
    const char*    kw_dat = (const char*)map_file(store + "/keyword/keyword.dat", &kw_dat_n);
    // title
    size_t t_kind_n = 0, t_py_n = 0, t_off_n = 0, t_dat_n = 0;
    const int32_t* t_kind = map_as<int32_t>(store + "/title/kind_id.bin", &t_kind_n);
    const int32_t* t_py   = map_as<int32_t>(store + "/title/production_year.bin", &t_py_n);
    const int64_t* t_off  = map_as<int64_t>(store + "/title/title.off", &t_off_n);
    const char*    t_dat  = (const char*)map_file(store + "/title/title.dat", &t_dat_n);
    // movie_keyword (sorted by movie_id)
    size_t mk_kid_n = 0;
    const int32_t* mk_kid = map_as<int32_t>(store + "/movie_keyword/keyword_id.bin", &mk_kid_n);
    // movie_info (sorted by movie_id)
    size_t mi_iti_n = 0, mi_off_n = 0, mi_dat_n = 0;
    const int32_t* mi_iti = map_as<int32_t>(store + "/movie_info/info_type_id.bin", &mi_iti_n);
    const int64_t* mi_off = map_as<int64_t>(store + "/movie_info/info.off", &mi_off_n);
    const char*    mi_dat = (const char*)map_file(store + "/movie_info/info.dat", &mi_dat_n);
    // movie_info_idx (sorted by movie_id)
    size_t mx_iti_n = 0, mx_off_n = 0, mx_dat_n = 0;
    const int32_t* mx_iti = map_as<int32_t>(store + "/movie_info_idx/info_type_id.bin", &mx_iti_n);
    const int64_t* mx_off = map_as<int64_t>(store + "/movie_info_idx/info.off", &mx_off_n);
    const char*    mx_dat = (const char*)map_file(store + "/movie_info_idx/info.dat", &mx_dat_n);
    // indexes
    size_t mkmid_n = 0, mimid_n = 0, mxmid_n = 0;
    const int32_t* mkmid = map_as<int32_t>(store + "/_idx/movie_keyword__movie_id__offsets.bin", &mkmid_n);
    const int32_t* mimid = map_as<int32_t>(store + "/_idx/movie_info__movie_id__offsets.bin", &mimid_n);
    const int32_t* mxmid = map_as<int32_t>(store + "/_idx/movie_info_idx__movie_id__offsets.bin", &mxmid_n);

    int32_t n_titles = (int32_t)t_kind_n;  // 2528312

    // ------------- Resolve dimensions -------------
    int32_t it1_id = -1, it2_id = -1, kt_id = -1;
    std::vector<int32_t> k_ids;
    {
        GENDB_PHASE("resolve_dims");
        int32_t n_it = (int32_t)(it_off_n - 1);
        for (int32_t r = 0; r < n_it; ++r) {
            const char* p = it_dat + it_off[r];
            size_t n = (size_t)(it_off[r+1] - it_off[r]);
            if (n == 9 && std::memcmp(p, "countries", 9) == 0) it1_id = r + 1;
            else if (n == 6 && std::memcmp(p, "rating", 6) == 0) it2_id = r + 1;
        }
        int32_t n_kt = (int32_t)(kt_off_n - 1);
        for (int32_t r = 0; r < n_kt; ++r) {
            const char* p = kt_dat + kt_off[r];
            size_t n = (size_t)(kt_off[r+1] - kt_off[r]);
            if (n == 5 && std::memcmp(p, "movie", 5) == 0) { kt_id = r + 1; break; }
        }
        int32_t n_kw = (int32_t)(kw_off_n - 1);
        for (int32_t r = 0; r < n_kw; ++r) {
            const char* p = kw_dat + kw_off[r];
            size_t n = (size_t)(kw_off[r+1] - kw_off[r]);
            if ((n == 6 && std::memcmp(p, "murder", 6) == 0) ||
                (n == 15 && std::memcmp(p, "murder-in-title", 15) == 0)) {
                k_ids.push_back(r + 1);
            }
        }
    }

    if (it1_id < 0 || it2_id < 0 || kt_id < 0 || k_ids.empty()) {
        std::fprintf(stderr, "dim resolution failed: it1=%d it2=%d kt=%d k_ids=%zu\n",
                     it1_id, it2_id, kt_id, k_ids.size());
    }

    // Country list — short strings; build a flat list with length+ptr.
    struct CStr { const char* p; uint8_t n; };
    static const char* COUNTRY_LIST[] = {
        "Sweden","Norway","Germany","Denmark","Swedish",
        "Denish","Norwegian","German","USA","American"
    };
    std::vector<CStr> countries;
    for (auto s : COUNTRY_LIST) countries.push_back({s, (uint8_t)std::strlen(s)});
    uint8_t country_min_len = 255, country_max_len = 0;
    for (auto& c : countries) {
        if (c.n < country_min_len) country_min_len = c.n;
        if (c.n > country_max_len) country_max_len = c.n;
    }

    auto title_match = [&](int32_t r) -> bool {
        const char* p = t_dat + t_off[r];
        size_t n = (size_t)(t_off[r+1] - t_off[r]);
        if (memmem(p, n, "murder", 6)) return true;
        if (memmem(p, n, "Murder", 6)) return true;
        if (memmem(p, n, "Mord", 4)) return true;
        return false;
    };

    // ------------- Main scan: title-driven with semi-joins and aggregation -------------
    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads <= 0) num_threads = 1;
    if (num_threads > 12) num_threads = 12;

    std::mutex agg_mtx;
    std::string g_min_rating;
    std::string g_min_title;
    bool g_have = false;

    auto worker = [&](int32_t lo, int32_t hi) {
        std::string th_min_rating;
        std::string th_min_title;
        bool th_have = false;

        for (int32_t r = lo; r < hi; ++r) {
            // cheap int filters first
            int32_t py = t_py[r];
            if (py == INT32_MIN || py <= 2010) continue;
            if (t_kind[r] != kt_id) continue;
            if (!title_match(r)) continue;

            int32_t mv = r + 1;

            // mk semi-join: any keyword_id in k_ids?
            int32_t mk_lo = mkmid[mv], mk_hi = mkmid[mv + 1];
            bool mk_ok = false;
            for (int32_t i = mk_lo; i < mk_hi; ++i) {
                int32_t kid = mk_kid[i];
                for (int32_t kk : k_ids) if (kid == kk) { mk_ok = true; break; }
                if (mk_ok) break;
            }
            if (!mk_ok) continue;

            // mi semi-join: any row with info_type_id==it1_id and info in country set?
            int32_t mi_lo = mimid[mv], mi_hi = mimid[mv + 1];
            bool mi_ok = false;
            for (int32_t i = mi_lo; i < mi_hi; ++i) {
                if (mi_iti[i] != it1_id) continue;
                const char* p = mi_dat + mi_off[i];
                size_t n = (size_t)(mi_off[i+1] - mi_off[i]);
                if (n < country_min_len || n > country_max_len) continue;
                for (auto& c : countries) {
                    if (c.n == n && std::memcmp(p, c.p, n) == 0) { mi_ok = true; break; }
                }
                if (mi_ok) break;
            }
            if (!mi_ok) continue;

            // mi_idx inner-join: scan and update mins for each matching row
            int32_t mx_lo = mxmid[mv], mx_hi = mxmid[mv + 1];
            for (int32_t i = mx_lo; i < mx_hi; ++i) {
                if (mx_iti[i] != it2_id) continue;
                const char* p = mx_dat + mx_off[i];
                size_t n = (size_t)(mx_off[i+1] - mx_off[i]);
                if (!lex_gt_60(p, n)) continue;

                std::string_view rating_sv(p, n);
                const char* tp = t_dat + t_off[r];
                size_t tn = (size_t)(t_off[r+1] - t_off[r]);
                std::string_view title_sv(tp, tn);

                if (!th_have) {
                    th_min_rating.assign(rating_sv);
                    th_min_title.assign(title_sv);
                    th_have = true;
                } else {
                    if (rating_sv < std::string_view(th_min_rating)) th_min_rating.assign(rating_sv);
                    if (title_sv < std::string_view(th_min_title)) th_min_title.assign(title_sv);
                }
            }
        }

        if (th_have) {
            std::lock_guard<std::mutex> lk(agg_mtx);
            if (!g_have) {
                g_min_rating = th_min_rating;
                g_min_title = th_min_title;
                g_have = true;
            } else {
                if (th_min_rating < g_min_rating) g_min_rating = th_min_rating;
                if (th_min_title < g_min_title) g_min_title = th_min_title;
            }
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        int32_t chunk = (n_titles + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; ++t) {
            int32_t lo = (int32_t)t * chunk;
            int32_t hi = std::min(lo + chunk, n_titles);
            if (lo >= hi) break;
            threads.emplace_back(worker, lo, hi);
        }
        for (auto& th : threads) th.join();
    }

    // ------------- Output -------------
    {
        GENDB_PHASE("output");
        mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q14b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "rating,western_dark_production\n");
        if (g_have) {
            std::fprintf(f, "%s,%s\n", g_min_rating.c_str(), g_min_title.c_str());
        }
        std::fclose(f);
    }

    return 0;
}
