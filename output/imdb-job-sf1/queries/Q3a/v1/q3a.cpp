// Q3a: SELECT MIN(t.title) FROM keyword k, movie_info mi, movie_keyword mk, title t
// WHERE k.keyword LIKE '%sequel%' AND mi.info IN (8 lang/country literals)
//   AND t.production_year > 2005 AND join keys equal.
//
// Strategy (from execution plan):
//   1. Scan keyword varlen → seq_ids (~30).
//   2. CSR walk movie_keyword__keyword_id → candidate movie ids (~10k unique).
//   3. Filter title.production_year > 2005 (~1500 survivors).
//   4. For each surviving movie, probe movie_info via offsets-only index; existence test on info IN-set.
//   5. Track MIN(t.title); emit.

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <sys/stat.h>
#include <climits>

using gendb::MmapColumn;

static const char* PATTERN = "sequel";
static const size_t PATTERN_LEN = 6;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];
    mkdir(results.c_str(), 0755);

    GENDB_PHASE("total");

    // ---- DATA LOADING ----
    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;
    MmapColumn<int32_t> mk_kid_off;   // CSR offsets (int32)
    MmapColumn<int32_t> mk_kid_rids;  // CSR rowids (int32)
    MmapColumn<int32_t> mk_movie_id;  // movie_keyword.movie_id
    MmapColumn<int32_t> t_prod_year;  // title.production_year
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;
    MmapColumn<int32_t> mi_movie_off; // offsets-only for movie_info by movie_id
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb + "/keyword/keyword.off");
        kw_dat.open(gendb + "/keyword/keyword.dat");
        mk_kid_off.open(gendb + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kid_rids.open(gendb + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb + "/movie_keyword/movie_id.bin");
        t_prod_year.open(gendb + "/title/production_year.bin");
        t_title_off.open(gendb + "/title/title.off");
        t_title_dat.open(gendb + "/title/title.dat");
        mi_movie_off.open(gendb + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off.open(gendb + "/movie_info/info.off");
        mi_info_dat.open(gendb + "/movie_info/info.dat");
        // Random access for offset-driven probes
        mk_movie_id.advise_random();
        t_prod_year.advise_random();
        mi_info_off.advise_random();
        mi_info_dat.advise_random();
    }

    // ---- Step 1: scan keyword for '%sequel%' ----
    std::vector<int32_t> seq_ids;
    {
        GENDB_PHASE("keyword_scan");
        size_t nk = kw_off.count - 1;  // 134170
        const char* dat = kw_dat.data;
        const int64_t* off = kw_off.data;
        for (size_t i = 0; i < nk; ++i) {
            int64_t lo = off[i];
            int64_t hi = off[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len < PATTERN_LEN) continue;
            if (memmem(dat + lo, len, PATTERN, PATTERN_LEN) != nullptr) {
                seq_ids.push_back((int32_t)(i + 1));  // dense identity: id = row+1
            }
        }
    }

    // ---- Step 2: CSR walk → candidate movies (unique) ----
    // Use a bitset over title id range for fast dedup; title rows = 2,528,312.
    const size_t TITLE_ROWS = t_prod_year.count;  // ~2.5M
    std::vector<uint64_t> seen_bits(TITLE_ROWS / 64 + 1, 0);
    std::vector<int32_t> candidate_movies;
    candidate_movies.reserve(16384);
    {
        GENDB_PHASE("csr_walk");
        const int32_t* off = mk_kid_off.data;
        const int32_t* rids = mk_kid_rids.data;
        const int32_t* mk_mv = mk_movie_id.data;
        for (int32_t kid : seq_ids) {
            int32_t lo = off[kid];
            int32_t hi = off[kid + 1];
            for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
                int32_t mk_row = rids[k_pos];
                int32_t mv = mk_mv[mk_row];
                if (mv <= 0 || (size_t)mv > TITLE_ROWS) continue;
                size_t idx = (size_t)(mv - 1);
                uint64_t mask = 1ULL << (idx & 63);
                uint64_t& w = seen_bits[idx >> 6];
                if (!(w & mask)) {
                    w |= mask;
                    candidate_movies.push_back(mv);
                }
            }
        }
    }

    // ---- Step 3: filter production_year > 2005 ----
    std::vector<int32_t> survivors;
    survivors.reserve(candidate_movies.size() / 4 + 16);
    {
        GENDB_PHASE("year_filter");
        const int32_t* py = t_prod_year.data;
        for (int32_t mv : candidate_movies) {
            int32_t y = py[mv - 1];
            if (y != INT32_MIN && y > 2005) {
                survivors.push_back(mv);
            }
        }
        std::sort(survivors.begin(), survivors.end());
    }

    // ---- Step 4: probe movie_info, IN-set existence test ----
    // 8 literals: Sweden(6), Norway(6), Germany(7), Denmark(7), Swedish(7), Denish(6), Norwegian(9), German(6)
    std::unordered_set<std::string_view> in_set;
    in_set.reserve(16);
    in_set.insert(std::string_view("Sweden", 6));
    in_set.insert(std::string_view("Norway", 6));
    in_set.insert(std::string_view("Germany", 7));
    in_set.insert(std::string_view("Denmark", 7));
    in_set.insert(std::string_view("Swedish", 7));
    in_set.insert(std::string_view("Denish", 6));
    in_set.insert(std::string_view("Norwegian", 9));
    in_set.insert(std::string_view("German", 6));

    std::string_view min_title;
    bool have_min = false;
    {
        GENDB_PHASE("main_scan");
        const int32_t* mi_off = mi_movie_off.data;
        const int64_t* info_off = mi_info_off.data;
        const char* info_dat = mi_info_dat.data;
        const int64_t* t_off = t_title_off.data;
        const char* t_dat = t_title_dat.data;

        for (int32_t mv : survivors) {
            int32_t lo = mi_off[mv];
            int32_t hi = mi_off[mv + 1];
            bool matched = false;
            for (int32_t r = lo; r < hi; ++r) {
                int64_t a = info_off[r];
                int64_t b = info_off[r + 1];
                size_t len = (size_t)(b - a);
                if (len != 6 && len != 7 && len != 9) continue;
                std::string_view sv(info_dat + a, len);
                if (in_set.find(sv) != in_set.end()) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                size_t idx = (size_t)(mv - 1);
                int64_t a = t_off[idx];
                int64_t b = t_off[idx + 1];
                std::string_view title(t_dat + a, (size_t)(b - a));
                if (!have_min || title < min_title) {
                    min_title = title;
                    have_min = true;
                }
            }
        }
    }

    // ---- OUTPUT ----
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q3a.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_title\n");
        if (have_min) {
            std::fwrite(min_title.data(), 1, min_title.size(), f);
            std::fputc('\n', f);
        } else {
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }

    return 0;
}
