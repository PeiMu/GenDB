// Q4a: SELECT MIN(mi_idx.info), MIN(t.title)
//      FROM info_type it, keyword k, movie_info_idx mi_idx, movie_keyword mk, title t
//      WHERE it.info='rating' AND k.keyword LIKE '%sequel%'
//        AND mi_idx.info > '5.0' AND t.production_year > 2005
//        AND t.id = mi_idx.movie_id AND t.id = mk.movie_id
//        AND k.id = mk.keyword_id AND it.id = mi_idx.info_type_id;

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static void ensure_dir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];
    ensure_dir(results);

    // --- mmap all columns ---
    MmapColumn<uint64_t> it_off, kw_off, t_title_off, mi_info_off;
    MmapColumn<char>     it_dat, kw_dat, t_title_dat, mi_info_dat;
    MmapColumn<int32_t>  mk_movie_id, mi_info_type_id, t_prod_year;
    MmapColumn<int32_t>  mk_kw_off, mk_kw_rowids, mi_mv_off;

    {
        GENDB_PHASE("data_loading");
        it_off.open(gendb + "/info_type/info.off");
        it_dat.open(gendb + "/info_type/info.dat");
        kw_off.open(gendb + "/keyword/keyword.off");
        kw_dat.open(gendb + "/keyword/keyword.dat");
        mk_movie_id.open(gendb + "/movie_keyword/movie_id.bin");
        mk_kw_off.open(gendb + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_rowids.open(gendb + "/_idx/movie_keyword__keyword_id__rowids.bin");
        t_prod_year.open(gendb + "/title/production_year.bin");
        t_title_off.open(gendb + "/title/title.off");
        t_title_dat.open(gendb + "/title/title.dat");
        mi_info_type_id.open(gendb + "/movie_info_idx/info_type_id.bin");
        mi_info_off.open(gendb + "/movie_info_idx/info.off");
        mi_info_dat.open(gendb + "/movie_info_idx/info.dat");
        mi_mv_off.open(gendb + "/_idx/movie_info_idx__movie_id__offsets.bin");
    }

    // --- resolve target_it_id = info_type where info='rating' ---
    int32_t target_it_id = -1;
    {
        GENDB_PHASE("resolve_it");
        const size_t n = it_off.count - 1;
        const char* dat = it_dat.data;
        const uint64_t* off = it_off.data;
        const char target[] = "rating";
        const size_t tlen = 6;
        for (size_t i = 0; i < n; ++i) {
            size_t lo = off[i], hi = off[i+1];
            if (hi - lo == tlen && std::memcmp(dat + lo, target, tlen) == 0) {
                target_it_id = (int32_t)(i + 1); // dense id = row+1
                break;
            }
        }
        if (target_it_id < 0) {
            std::fprintf(stderr, "info_type 'rating' not found\n");
            return 2;
        }
    }

    // --- scan keyword for LIKE '%sequel%' ---
    std::vector<int32_t> seq_ids;
    seq_ids.reserve(128);
    {
        GENDB_PHASE("scan_keyword");
        const size_t n = kw_off.count - 1;
        const char* dat = kw_dat.data;
        const uint64_t* off = kw_off.data;
        const char needle[] = "sequel";
        const size_t nlen = 6;
        for (size_t i = 0; i < n; ++i) {
            size_t lo = off[i], hi = off[i+1];
            size_t len = hi - lo;
            if (len < nlen) continue;
            if (memmem(dat + lo, len, needle, nlen) != nullptr) {
                seq_ids.push_back((int32_t)(i + 1));
            }
        }
    }

    // --- enumerate candidate movies via movie_keyword__keyword_id CSR ---
    // dedupe via seen_bitmap sized to title rows
    const size_t title_n = t_prod_year.count;  // 2528312
    std::vector<uint8_t> seen(title_n + 2, 0);  // index by movie_id (1..title_n)
    std::vector<int32_t> candidates;
    candidates.reserve(20000);
    {
        GENDB_PHASE("probe_mk_csr");
        const int32_t* off = mk_kw_off.data;
        const int32_t* rids = mk_kw_rowids.data;
        const int32_t* mk_mv = mk_movie_id.data;
        const size_t off_count = mk_kw_off.count; // 134172
        for (int32_t kid : seq_ids) {
            if (kid < 1 || (size_t)(kid + 1) >= off_count) continue;
            int32_t lo = off[kid], hi = off[kid + 1];
            for (int32_t p = lo; p < hi; ++p) {
                int32_t mk_row = rids[p];
                int32_t mv = mk_mv[mk_row];
                if (mv < 1 || (size_t)mv > title_n) continue;
                if (!seen[mv]) {
                    seen[mv] = 1;
                    candidates.push_back(mv);
                }
            }
        }
    }

    // --- filter by production_year > 2005, then probe mi_idx by movie_id ---
    std::string_view min_rating;
    std::string_view min_title;
    bool have_min = false;
    int64_t survivors = 0;
    {
        GENDB_PHASE("main_scan");
        const int32_t* py = t_prod_year.data;
        const uint64_t* tit_off = t_title_off.data;
        const char*     tit_dat = t_title_dat.data;
        const int32_t*  mi_off  = mi_mv_off.data;
        const int32_t*  mi_iti  = mi_info_type_id.data;
        const uint64_t* mi_io   = mi_info_off.data;
        const char*     mi_id   = mi_info_dat.data;
        const size_t    mi_off_count = mi_mv_off.count; // title_n + 2

        for (int32_t mv : candidates) {
            // production_year filter (mv is title id; title row = mv-1)
            int32_t y = py[mv - 1];
            if (y == INT32_MIN || y <= 2005) continue;

            // mi_idx row range for this movie
            if ((size_t)(mv + 1) >= mi_off_count) continue;
            int32_t lo = mi_off[mv], hi = mi_off[mv + 1];
            if (lo == hi) continue;

            // For each mi_idx row: info_type_id == target_it_id && info > "5.0"
            bool movie_qualifies = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (mi_iti[r] != target_it_id) continue;
                size_t slo = mi_io[r], shi = mi_io[r + 1];
                size_t slen = shi - slo;
                if (slen == 0) continue;
                std::string_view info(mi_id + slo, slen);
                // lexicographic > "5.0"
                if (info <= std::string_view("5.0", 3)) continue;
                ++survivors;
                if (!have_min || info < min_rating) {
                    min_rating = info;
                }
                movie_qualifies = true;
            }
            if (movie_qualifies) {
                size_t tlo = tit_off[mv - 1], thi = tit_off[mv];
                std::string_view title(tit_dat + tlo, thi - tlo);
                if (!have_min || title < min_title) {
                    min_title = title;
                }
                have_min = true;
            }
        }
    }

    // --- output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q4a.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "rating,movie_title\n");
        if (have_min) {
            std::fwrite(min_rating.data(), 1, min_rating.size(), f);
            std::fputc(',', f);
            std::fwrite(min_title.data(), 1, min_title.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    std::fprintf(stderr, "[Q4a] candidates=%zu survivors=%lld target_it=%d seq_keywords=%zu\n",
                 candidates.size(), (long long)survivors, target_it_id, seq_ids.size());
    return 0;
}
