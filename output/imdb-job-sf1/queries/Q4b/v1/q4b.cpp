// Q4b: MIN(mi_idx.info), MIN(t.title) WHERE info_type='rating', keyword LIKE '%sequel%',
//      info > '9.0', production_year > 2010
//
// Plan:
//  1. Scan info_type to resolve target_it_id (where info='rating').
//  2. Scan keyword to find seq_ids (keyword LIKE '%sequel%') via memmem.
//  3. Probe movie_keyword__keyword_id CSR to collect candidate movies.
//  4. Filter candidates by title.production_year > 2010.
//  5. For each surviving movie, iterate movie_info_idx rows via movie_id offsets;
//     filter info_type_id == target_it_id and info > "9.0" lex; track MIN(info), MIN(title).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <climits>
#include <filesystem>
#include <fstream>

#define _GNU_SOURCE
#include <string.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    GENDB_PHASE("total");

    // ------------------------------------------------------------------
    // Phase 1: data loading (mmap)
    // ------------------------------------------------------------------
    MmapColumn<int32_t> it_id;
    MmapColumn<uint64_t> it_info_off;
    MmapColumn<char> it_info_dat;

    MmapColumn<int32_t> kw_id;
    MmapColumn<uint64_t> kw_off;
    MmapColumn<char> kw_dat;

    MmapColumn<int32_t> mk_off;       // CSR offsets indexed by keyword_id
    MmapColumn<int32_t> mk_rowids;    // CSR rowids
    MmapColumn<int32_t> mk_movie_id;  // movie_keyword.movie_id

    MmapColumn<int32_t> t_prodyear;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int32_t> mi_off;       // movie_info_idx__movie_id offsets (indexed by title.id)
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<uint64_t> mi_info_off;
    MmapColumn<char>     mi_info_dat;

    {
        GENDB_PHASE("data_loading");
        it_id.open(gendb_dir + "/info_type/id.bin");
        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");

        kw_id.open(gendb_dir + "/keyword/id.bin");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        mk_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");

        t_prodyear.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mi_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");

        // Prefetch the large mi_idx info column since we'll probe it randomly.
        mi_info_dat.advise_random();
        mi_info_off.advise_random();
        t_title_dat.advise_random();
        t_title_off.advise_random();
        mk_rowids.advise_sequential();
    }

    // ------------------------------------------------------------------
    // Phase 2: resolve target_it_id (info_type.info = 'rating')
    // ------------------------------------------------------------------
    int32_t target_it_id = -1;
    {
        GENDB_PHASE("resolve_info_type");
        const size_t n = it_id.count;
        const char* dat = it_info_dat.data;
        for (size_t r = 0; r < n; ++r) {
            size_t s = it_info_off[r];
            size_t e = it_info_off[r + 1];
            size_t len = e - s;
            if (len == 6 && std::memcmp(dat + s, "rating", 6) == 0) {
                target_it_id = it_id[r];
                break;
            }
        }
        if (target_it_id < 0) {
            std::fprintf(stderr, "info_type 'rating' not found\n");
            return 2;
        }
    }

    // ------------------------------------------------------------------
    // Phase 3: keyword LIKE '%sequel%'  → seq_ids
    // ------------------------------------------------------------------
    std::vector<int32_t> seq_ids;
    seq_ids.reserve(64);
    {
        GENDB_PHASE("scan_keyword");
        const size_t n = kw_id.count;
        const char* dat = kw_dat.data;
        const char needle[] = "sequel";
        const size_t nlen = 6;
        for (size_t r = 0; r < n; ++r) {
            size_t s = kw_off[r];
            size_t e = kw_off[r + 1];
            size_t len = e - s;
            if (len < nlen) continue;
            if (memmem(dat + s, len, needle, nlen) != nullptr) {
                seq_ids.push_back(kw_id[r]);
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: probe movie_keyword__keyword_id CSR  → candidate_movies
    // ------------------------------------------------------------------
    std::unordered_set<int32_t> candidate_movies;
    candidate_movies.reserve(8192);
    {
        GENDB_PHASE("probe_mk_csr");
        for (int32_t kid : seq_ids) {
            int32_t lo = mk_off[kid];
            int32_t hi = mk_off[kid + 1];
            for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
                int32_t mk_row = mk_rowids[k_pos];
                int32_t mv = mk_movie_id[mk_row];
                candidate_movies.insert(mv);
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase 5: filter by title.production_year > 2010
    // ------------------------------------------------------------------
    std::vector<int32_t> surviving;
    surviving.reserve(candidate_movies.size());
    {
        GENDB_PHASE("filter_prod_year");
        const int32_t* py = t_prodyear.data;
        for (int32_t mv : candidate_movies) {
            // title.id is 1-indexed, dense → row index = mv - 1
            int32_t y = py[mv - 1];
            if (y != INT32_MIN && y > 2010) {
                surviving.push_back(mv);
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase 6: probe movie_info_idx; filter; track MIN(info), MIN(title)
    // ------------------------------------------------------------------
    // Track minima as offset+len into the underlying .dat (avoid copies)
    bool have_result = false;
    uint64_t best_info_s = 0, best_info_e = 0;
    uint64_t best_title_s = 0, best_title_e = 0;
    const char* mi_dat = mi_info_dat.data;
    const char* t_dat  = t_title_dat.data;

    {
        GENDB_PHASE("main_scan");
        const int32_t target = target_it_id;
        const char nine_dot_zero[] = "9.0";
        for (int32_t mv : surviving) {
            int32_t lo = mi_off[mv];
            int32_t hi = mi_off[mv + 1];
            for (int32_t r = lo; r < hi; ++r) {
                if (mi_info_type_id[r] != target) continue;
                uint64_t is = mi_info_off[r];
                uint64_t ie = mi_info_off[r + 1];
                size_t ilen = ie - is;
                if (ilen == 0) continue;  // NULL
                // Compare info > "9.0" lex
                size_t cmplen = ilen < 3 ? ilen : 3;
                int c = std::memcmp(mi_dat + is, nine_dot_zero, cmplen);
                if (c < 0) continue;
                if (c == 0 && ilen <= 3) continue;  // equal or shorter prefix → not > "9.0"
                // Qualifies. Track MIN(info)
                if (!have_result) {
                    best_info_s = is; best_info_e = ie;
                    best_title_s = t_title_off[mv - 1];
                    best_title_e = t_title_off[mv];
                    have_result = true;
                } else {
                    // info compare
                    size_t bl = best_info_e - best_info_s;
                    size_t ml = ilen < bl ? ilen : bl;
                    int cc = std::memcmp(mi_dat + is, mi_dat + best_info_s, ml);
                    if (cc < 0 || (cc == 0 && ilen < bl)) {
                        best_info_s = is; best_info_e = ie;
                    }
                    // title compare
                    uint64_t ts = t_title_off[mv - 1];
                    uint64_t te = t_title_off[mv];
                    size_t tl = te - ts;
                    size_t bbl = best_title_e - best_title_s;
                    size_t mml = tl < bbl ? tl : bbl;
                    int tc = std::memcmp(t_dat + ts, t_dat + best_title_s, mml);
                    if (tc < 0 || (tc == 0 && tl < bbl)) {
                        best_title_s = ts; best_title_e = te;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Output
    // ------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::filesystem::create_directories(results_dir);
        std::string out_path = results_dir + "/Q4b.csv";
        std::ofstream out(out_path);
        out << "rating,movie_title\n";
        if (have_result) {
            out.write(mi_dat + best_info_s, best_info_e - best_info_s);
            out << ",";
            out.write(t_dat + best_title_s, best_title_e - best_title_s);
            out << "\n";
        }
        out.close();
    }

    return 0;
}
