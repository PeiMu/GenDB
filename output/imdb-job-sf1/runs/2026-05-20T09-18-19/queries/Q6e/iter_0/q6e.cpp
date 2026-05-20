// Q6e: Marvel cinematic universe movies starring Robert Downey
// SELECT MIN(k.keyword), MIN(n.name), MIN(t.title)
//   FROM cast_info ci, keyword k, movie_keyword mk, name n, title t
//  WHERE k.keyword = 'marvel-cinematic-universe'
//    AND n.name LIKE '%Downey%Robert%'
//    AND t.production_year > 2000
//    AND k.id = mk.keyword_id AND t.id = mk.movie_id
//    AND t.id = ci.movie_id AND n.id = ci.person_id;

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

namespace fs = std::filesystem;

// LIKE '%Downey%Robert%' — check if both "Downey" and "Robert" appear in order
static inline bool name_matches(const char* s, size_t len) {
    static constexpr const char DOWNEY[] = "Downey";
    static constexpr const char ROBERT[] = "Robert";
    static constexpr size_t DLEN = 6;
    static constexpr size_t RLEN = 6;
    if (len < DLEN + RLEN) return false;
    // search for "Downey"
    const char* p = s;
    const char* end = s + len;
    const char* dp = nullptr;
    for (; p + DLEN <= end; ++p) {
        if (std::memcmp(p, DOWNEY, DLEN) == 0) { dp = p + DLEN; break; }
    }
    if (!dp) return false;
    // search for "Robert" after Downey
    for (const char* q = dp; q + RLEN <= end; ++q) {
        if (std::memcmp(q, ROBERT, RLEN) == 0) return true;
    }
    return false;
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

    // ---------------------------------------------------------------
    // Phase 1: mmap all needed files
    // ---------------------------------------------------------------
    MmapColumn<int64_t> kw_off;       // keyword.off (N+1 int64 offsets into keyword.dat)
    MmapColumn<char>    kw_dat;       // keyword.dat

    MmapColumn<int32_t> mk_off_idx;   // _idx/movie_keyword__keyword_id__offsets.bin
    MmapColumn<int32_t> mk_rowids;    // _idx/movie_keyword__keyword_id__rowids.bin
    MmapColumn<int32_t> mk_movie_id;  // movie_keyword/movie_id.bin

    MmapColumn<int32_t> py_bin;       // title/production_year.bin
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;

    MmapColumn<int32_t> ci_off_idx;   // _idx/cast_info__movie_id__offsets.bin
    MmapColumn<int32_t> ci_person_id; // cast_info/person_id.bin

    MmapColumn<int64_t> name_off;
    MmapColumn<char>    name_dat;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        mk_off_idx.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");

        py_bin.open(gendb_dir + "/title/production_year.bin");
        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");

        ci_off_idx.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");

        name_off.open(gendb_dir + "/name/name.off");
        name_dat.open(gendb_dir + "/name/name.dat");

        // Prefetch big files
        ci_person_id.prefetch();
        name_off.prefetch();
        name_dat.prefetch();
    }

    // ---------------------------------------------------------------
    // Phase 2: Resolve target_k_id from keyword.keyword
    // ---------------------------------------------------------------
    int32_t target_k_id = -1;
    {
        GENDB_PHASE("keyword_resolve");
        static const char K[] = "marvel-cinematic-universe";
        static const size_t KLEN = sizeof(K) - 1;
        size_t n_keywords = kw_off.size() - 1; // 134170
        const int64_t* off = kw_off.data;
        const char* dat = kw_dat.data;
        for (size_t i = 0; i < n_keywords; ++i) {
            int64_t lo = off[i], hi = off[i+1];
            if (hi - lo == (int64_t)KLEN && std::memcmp(dat + lo, K, KLEN) == 0) {
                target_k_id = (int32_t)(i + 1); // dense PK, 1-based id
                break;
            }
        }
        if (target_k_id < 0) {
            std::fprintf(stderr, "keyword not found\n");
            // Still write empty result
        }
    }

    // ---------------------------------------------------------------
    // Phase 3: Build downey_robert_ids by parallel scan of name.name
    // ---------------------------------------------------------------
    std::unordered_set<int32_t> downey_robert_ids;
    {
        GENDB_PHASE("name_filter");
        size_t n_names = name_off.size() - 1; // 4167491
        const int64_t* off = name_off.data;
        const char* dat = name_dat.data;

        const int num_threads = 12;
        std::vector<std::vector<int32_t>> per_thread(num_threads);

        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (int t = 0; t < num_threads; ++t) {
            size_t start = (n_names * t) / num_threads;
            size_t end   = (n_names * (t + 1)) / num_threads;
            auto& vec = per_thread[t];
            for (size_t i = start; i < end; ++i) {
                int64_t lo = off[i], hi = off[i+1];
                size_t len = (size_t)(hi - lo);
                if (name_matches(dat + lo, len)) {
                    vec.push_back((int32_t)(i + 1));
                }
            }
        }

        size_t total = 0;
        for (auto& v : per_thread) total += v.size();
        downey_robert_ids.reserve(total * 2);
        for (auto& v : per_thread) {
            for (int32_t id : v) downey_robert_ids.insert(id);
        }
    }

    // ---------------------------------------------------------------
    // Phase 4: Probe CSR for target_k_id -> mk_rows -> t_ids (year filter)
    // ---------------------------------------------------------------
    std::vector<int32_t> survivor_t_ids;
    if (target_k_id > 0) {
        GENDB_PHASE("mk_probe_year_filter");
        int32_t lo = mk_off_idx.data[target_k_id];
        int32_t hi = mk_off_idx.data[target_k_id + 1];
        survivor_t_ids.reserve(hi - lo);
        const int32_t* py = py_bin.data;
        for (int32_t j = lo; j < hi; ++j) {
            int32_t mk_row = mk_rowids.data[j];
            int32_t t_id = mk_movie_id.data[mk_row];
            int32_t year = py[t_id - 1];
            if (year != INT32_MIN && year > 2000) {
                survivor_t_ids.push_back(t_id);
            }
        }
        // Distinct (likely already, but ensure)
        std::sort(survivor_t_ids.begin(), survivor_t_ids.end());
        survivor_t_ids.erase(std::unique(survivor_t_ids.begin(), survivor_t_ids.end()),
                             survivor_t_ids.end());
    }

    // ---------------------------------------------------------------
    // Phase 5: For each surviving t_id, scan ci range, semi-join name set
    //          Track MIN(t.title) and MIN(n.name)
    // ---------------------------------------------------------------
    int32_t best_t_id = -1;
    std::string best_title;
    std::string best_name;
    {
        GENDB_PHASE("main_scan");
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int64_t* t_off = title_off.data;
        const char* t_dat = title_dat.data;
        const int64_t* n_off = name_off.data;
        const char* n_dat = name_dat.data;

        for (int32_t t_id : survivor_t_ids) {
            int32_t clo = ci_off[t_id];
            int32_t chi = ci_off[t_id + 1];
            // get title once for comparison (we may match many actors per t_id)
            std::string_view title_sv(t_dat + t_off[t_id - 1],
                                      (size_t)(t_off[t_id] - t_off[t_id - 1]));
            for (int32_t r = clo; r < chi; ++r) {
                int32_t pid = ci_pid[r];
                if (downey_robert_ids.find(pid) != downey_robert_ids.end()) {
                    std::string_view name_sv(n_dat + n_off[pid - 1],
                                             (size_t)(n_off[pid] - n_off[pid - 1]));
                    // Update MIN(name)
                    if (best_name.empty() || name_sv < best_name) {
                        best_name.assign(name_sv);
                    }
                    // Update MIN(title)
                    if (best_title.empty() || title_sv < best_title) {
                        best_title.assign(title_sv);
                        best_t_id = t_id;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Phase 6: Output
    // ---------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q6e.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(fp, "movie_keyword,actor_name,marvel_movie\n");
        if (!best_title.empty() && !best_name.empty()) {
            // CSV-quote the name if it contains comma
            auto quote_if_needed = [](const std::string& s) -> std::string {
                bool needs = false;
                for (char c : s) if (c == ',' || c == '"' || c == '\n') { needs = true; break; }
                if (!needs) return s;
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += "\"";
                return out;
            };
            std::fprintf(fp, "%s,%s,%s\n",
                         quote_if_needed("marvel-cinematic-universe").c_str(),
                         quote_if_needed(best_name).c_str(),
                         quote_if_needed(best_title).c_str());
        }
        std::fclose(fp);
    }

    return 0;
}
