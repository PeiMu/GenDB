// Q32b: SELECT MIN(lt.link), MIN(t1.title), MIN(t2.title)
//       FROM keyword k, link_type lt, movie_keyword mk, movie_link ml,
//            title t1, title t2
//       WHERE k.keyword='character-name-in-title' AND
//             mk.keyword_id=k.id AND t1.id=mk.movie_id AND
//             ml.movie_id=t1.id AND ml.linked_movie_id=t2.id AND
//             lt.id=ml.link_type_id;

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static const char* KEYWORD_LITERAL = "character-name-in-title";
static const int   KEYWORD_LEN     = 23;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---------------- Data loading: mmap all needed columns ----------------
    MmapColumn<int64_t> k_off;
    MmapColumn<char>    k_dat;
    MmapColumn<int32_t> mk_kw_offsets;   // CSR offsets per keyword_id
    MmapColumn<int32_t> mk_kw_rowids;    // CSR rowids -> rows in movie_keyword
    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> ml_movie_id;
    MmapColumn<int32_t> ml_linked_movie_id;
    MmapColumn<int32_t> ml_link_type_id;
    MmapColumn<int64_t> t_off;
    MmapColumn<char>    t_dat;
    MmapColumn<int64_t> lt_off;
    MmapColumn<char>    lt_dat;

    {
        GENDB_PHASE("data_loading");
        k_off.open(gendb_dir + "/keyword/keyword.off");
        k_dat.open(gendb_dir + "/keyword/keyword.dat");
        mk_kw_offsets.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
        ml_movie_id.open(gendb_dir + "/movie_link/movie_id.bin");
        ml_linked_movie_id.open(gendb_dir + "/movie_link/linked_movie_id.bin");
        ml_link_type_id.open(gendb_dir + "/movie_link/link_type_id.bin");
        t_off.open(gendb_dir + "/title/title.off");
        t_dat.open(gendb_dir + "/title/title.dat");
        lt_off.open(gendb_dir + "/link_type/link.off");
        lt_dat.open(gendb_dir + "/link_type/link.dat");
    }

    // ---------------- Resolve keyword -> k_id ----------------
    int32_t k_id = -1;
    {
        GENDB_PHASE("keyword_scan");
        const int64_t* off = k_off.data;
        const char* dat = k_dat.data;
        size_t n = (k_off.count > 0) ? (k_off.count - 1) : 0;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = off[i], hi = off[i+1];
            if ((hi - lo) != KEYWORD_LEN) continue;
            if (std::memcmp(dat + lo, KEYWORD_LITERAL, KEYWORD_LEN) == 0) {
                // keyword.id is identity (1-based -> row i is id i+1)
                k_id = (int32_t)(i + 1);
                break;
            }
        }
        if (k_id < 0) {
            std::fprintf(stderr, "keyword not found\n");
            FILE* fp = std::fopen((results_dir + "/Q32b.csv").c_str(), "w");
            if (fp) {
                std::fprintf(fp, "link_type,first_movie,second_movie\n");
                std::fclose(fp);
            }
            return 0;
        }
    }

    // ---------------- Build t1_ids hash set from CSR slot ----------------
    std::unordered_set<int32_t> t1_ids;
    {
        GENDB_PHASE("build_t1_set");
        int32_t slot_lo = mk_kw_offsets[k_id];
        int32_t slot_hi = mk_kw_offsets[k_id + 1];
        size_t slot_size = (size_t)(slot_hi - slot_lo);
        t1_ids.reserve(slot_size * 2 + 16);
        const int32_t* rowids = mk_kw_rowids.data;
        const int32_t* mvids = mk_movie_id.data;
        for (int32_t r = slot_lo; r < slot_hi; ++r) {
            int32_t row = rowids[r];
            int32_t mid = mvids[row];
            t1_ids.insert(mid);
        }
    }

    // ---------------- Linear scan movie_link, probe t1_ids ----------------
    // Track three running MINs.
    std::string min_lt_link;
    std::string min_t1_title;
    std::string min_t2_title;
    bool have_any = false;

    auto sv_at = [](const int64_t* off, const char* dat, int32_t idx) -> std::string_view {
        int64_t lo = off[idx], hi = off[idx + 1];
        return std::string_view(dat + lo, (size_t)(hi - lo));
    };

    {
        GENDB_PHASE("main_scan");
        const int32_t* mvid = ml_movie_id.data;
        const int32_t* lmvid = ml_linked_movie_id.data;
        const int32_t* lt_id_col = ml_link_type_id.data;
        const size_t n_ml = ml_movie_id.count;
        const int64_t* toff = t_off.data;
        const char* tdat = t_dat.data;
        const int64_t* ltoff = lt_off.data;
        const char* ltdat = lt_dat.data;

        for (size_t r = 0; r < n_ml; ++r) {
            int32_t t1_id = mvid[r];
            if (t1_ids.find(t1_id) == t1_ids.end()) continue;
            int32_t t2_id = lmvid[r];
            int32_t lt_id = lt_id_col[r];

            // title.id is identity (1-based) so row index = id - 1
            std::string_view t1sv = sv_at(toff, tdat, t1_id - 1);
            std::string_view t2sv = sv_at(toff, tdat, t2_id - 1);
            std::string_view ltsv = sv_at(ltoff, ltdat, lt_id - 1);

            if (!have_any) {
                min_lt_link.assign(ltsv);
                min_t1_title.assign(t1sv);
                min_t2_title.assign(t2sv);
                have_any = true;
            } else {
                if (ltsv < std::string_view(min_lt_link)) min_lt_link.assign(ltsv);
                if (t1sv < std::string_view(min_t1_title)) min_t1_title.assign(t1sv);
                if (t2sv < std::string_view(min_t2_title)) min_t2_title.assign(t2sv);
            }
        }
    }

    // ---------------- Output CSV ----------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q32b.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open output: %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(fp, "link_type,first_movie,second_movie\n");
        if (have_any) {
            // Simple CSV write — values in the IMDb dataset for these MIN columns
            // don't contain commas/quotes/newlines per the ground truth, but to be
            // safe, wrap fields containing comma/quote/newline in quotes and escape "".
            auto write_field = [&](const std::string& s) {
                bool need_quote = false;
                for (char c : s) {
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
                }
                if (!need_quote) {
                    std::fwrite(s.data(), 1, s.size(), fp);
                } else {
                    std::fputc('"', fp);
                    for (char c : s) {
                        if (c == '"') std::fputc('"', fp);
                        std::fputc(c, fp);
                    }
                    std::fputc('"', fp);
                }
            };
            write_field(min_lt_link);
            std::fputc(',', fp);
            write_field(min_t1_title);
            std::fputc(',', fp);
            write_field(min_t2_title);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
