// Q3c — MIN(t.title) with star join: keyword(LIKE '%sequel%') -> mk -> title(prod_year>1990) -> mi(info IN ...)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static int ensure_dir(const std::string& path) {
    return mkdir(path.c_str(), 0755);
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    ensure_dir(results_dir);

    // mmap inputs
    MmapColumn<uint64_t> kw_off;
    MmapColumn<char>     kw_dat;
    MmapColumn<int32_t>  mk_kid_off;       // CSR offsets (id-indexed)
    MmapColumn<int32_t>  mk_kid_rowids;
    MmapColumn<int32_t>  mk_movie_id;
    MmapColumn<int32_t>  prod_year;
    MmapColumn<uint64_t> title_off;
    MmapColumn<char>     title_dat;
    MmapColumn<int32_t>  mi_mv_off;        // offsets_only (id-indexed)
    MmapColumn<int32_t>  mi_movie_id;
    MmapColumn<uint64_t> mi_info_off;
    MmapColumn<char>     mi_info_dat;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");
        mk_kid_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kid_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
        prod_year.open(gendb_dir + "/title/production_year.bin");
        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");
        mi_mv_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_movie_id.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
    }

    // Phase 1: scan keyword for LIKE '%sequel%'
    std::vector<int32_t> seq_ids;
    seq_ids.reserve(128);
    const size_t kw_rows = kw_off.count - 1; // rows = entries - 1 (sentinel)
    {
        GENDB_PHASE("scan_keyword_like");
        const char* dat = kw_dat.data;
        const uint64_t* off = kw_off.data;
        for (size_t i = 0; i < kw_rows; ++i) {
            uint64_t lo = off[i];
            uint64_t hi = off[i + 1];
            size_t len = hi - lo;
            if (len < 6) continue;
            if (memmem(dat + lo, len, "sequel", 6) != nullptr) {
                // keyword.id is dense 1-based -> id = i + 1
                seq_ids.push_back(static_cast<int32_t>(i + 1));
            }
        }
    }

    // Phase 2: expand keyword ids -> candidate movie set via CSR
    std::unordered_set<int32_t> candidate_movies;
    candidate_movies.reserve(200000);
    {
        GENDB_PHASE("csr_lookup_movie_keyword");
        const int32_t* off = mk_kid_off.data;
        const int32_t* rowids = mk_kid_rowids.data;
        const int32_t* mv = mk_movie_id.data;
        for (int32_t kid : seq_ids) {
            int32_t lo = off[kid];
            int32_t hi = off[kid + 1];
            for (int32_t kp = lo; kp < hi; ++kp) {
                int32_t mkr = rowids[kp];
                candidate_movies.insert(mv[mkr]);
            }
        }
    }

    // Phase 3: filter candidates by production_year > 1990 (title.id is dense 1-based)
    std::vector<int32_t> surviving;
    surviving.reserve(candidate_movies.size());
    {
        GENDB_PHASE("filter_title_production_year");
        const int32_t* py = prod_year.data;
        const size_t n_titles = prod_year.count;
        for (int32_t id : candidate_movies) {
            size_t row = static_cast<size_t>(id) - 1;
            if (row >= n_titles) continue;
            int32_t y = py[row];
            if (y != INT32_MIN && y > 1990) surviving.push_back(id);
        }
    }

    // Phase 4: for each surviving movie, walk movie_info rows, test info IN-set
    // IN-set: 'Sweden'(6), 'Norway'(6), 'Germany'(7), 'Denmark'(7),
    //         'Swedish'(7), 'Denish'(6), 'Norwegian'(9), 'German'(6),
    //         'USA'(3), 'American'(8)
    // length distribution: 3,6,6,6,6,6,7,7,7,8,9
    static const char* const literals[] = {
        "Sweden", "Norway", "Germany", "Denmark",
        "Swedish", "Denish", "Norwegian", "German",
        "USA", "American"
    };
    static const size_t lit_lens[] = {6,6,7,7,7,6,9,6,3,8};
    constexpr int N_LIT = 10;

    // Build flat hash set for fast lookup
    std::unordered_set<std::string_view> in_set;
    in_set.reserve(32);
    for (int i = 0; i < N_LIT; ++i) {
        in_set.emplace(literals[i], lit_lens[i]);
    }
    // Length bitmap: only lengths 3,6,7,8,9 can match
    auto valid_len = [](size_t L) {
        return L == 3 || L == 6 || L == 7 || L == 8 || L == 9;
    };

    std::string_view best_title;
    bool have_best = false;

    {
        GENDB_PHASE("main_scan");
        const int32_t* mi_off_arr = mi_mv_off.data;
        const uint64_t* info_off = mi_info_off.data;
        const char* info_dat = mi_info_dat.data;
        const uint64_t* t_off = title_off.data;
        const char* t_dat = title_dat.data;
        const size_t mi_off_count = mi_mv_off.count;

        for (int32_t mv : surviving) {
            size_t idx = static_cast<size_t>(mv);
            if (idx + 1 >= mi_off_count) continue;
            int32_t lo = mi_off_arr[idx];
            int32_t hi = mi_off_arr[idx + 1];
            bool matched = false;
            for (int32_t r = lo; r < hi; ++r) {
                uint64_t s = info_off[r];
                uint64_t e = info_off[r + 1];
                size_t L = static_cast<size_t>(e - s);
                if (!valid_len(L)) continue;
                std::string_view sv(info_dat + s, L);
                if (in_set.find(sv) != in_set.end()) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;

            // Read title
            size_t trow = static_cast<size_t>(mv) - 1;
            uint64_t ts = t_off[trow];
            uint64_t te = t_off[trow + 1];
            std::string_view tv(t_dat + ts, te - ts);
            if (!have_best || tv < best_title) {
                best_title = tv;
                have_best = true;
            }
        }
    }

    // Output
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q3c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open output %s\n", out_path.c_str());
            return 2;
        }
        std::fprintf(f, "movie_title\n");
        if (have_best) {
            std::fwrite(best_title.data(), 1, best_title.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
