// Q7b: SELECT MIN(n.name), MIN(t.title)
// Pipeline: resolve dims → pi(info_type, note) → name(gender,pcode) → aka(LIKE) →
//           ci(movie_id) → title(year in 1980-1984) → ml(link_type='features')
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// CSV-quote a string if it contains comma or quote.
static std::string csv_quote(std::string_view s) {
    bool needs = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n') { needs = true; break; }
    }
    if (!needs) return std::string(s);
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // ---- Data loading (mmap) ----
    // info_type
    MmapColumn<int64_t> it_info_off;
    MmapColumn<char>    it_info_dat;
    // link_type
    MmapColumn<int64_t> lt_link_off;
    MmapColumn<char>    lt_link_dat;
    // person_info
    MmapColumn<int32_t> pi_person_id;
    MmapColumn<int64_t> pi_note_off;
    MmapColumn<char>    pi_note_dat;
    // name
    MmapColumn<int64_t> n_name_off;
    MmapColumn<char>    n_name_dat;
    MmapColumn<int64_t> n_pcf_off;
    MmapColumn<char>    n_pcf_dat;
    MmapColumn<int8_t>  n_gender_bin;
    MmapColumn<int64_t> n_gdict_off;
    MmapColumn<char>    n_gdict_dat;
    // aka_name
    MmapColumn<int64_t> an_name_off;
    MmapColumn<char>    an_name_dat;
    // cast_info
    MmapColumn<int32_t> ci_movie_id;
    // movie_link
    MmapColumn<int32_t> ml_link_type_id;
    // title
    MmapColumn<int32_t> t_prod_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;
    // Indexes
    MmapColumn<int32_t> pi_it_off;
    MmapColumn<int32_t> pi_it_rowids;
    MmapColumn<int32_t> an_off;
    MmapColumn<int32_t> ci_p_off;
    MmapColumn<int32_t> ci_p_rowids;
    MmapColumn<int32_t> ml_lm_off;
    MmapColumn<int32_t> ml_lm_rowids;

    {
        GENDB_PHASE("data_loading");
        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");
        lt_link_off.open(gendb_dir + "/link_type/link.off");
        lt_link_dat.open(gendb_dir + "/link_type/link.dat");
        pi_person_id.open(gendb_dir + "/person_info/person_id.bin");
        pi_note_off.open(gendb_dir + "/person_info/note.off");
        pi_note_dat.open(gendb_dir + "/person_info/note.dat");
        n_name_off.open(gendb_dir + "/name/name.off");
        n_name_dat.open(gendb_dir + "/name/name.dat");
        n_pcf_off.open(gendb_dir + "/name/name_pcode_cf.off");
        n_pcf_dat.open(gendb_dir + "/name/name_pcode_cf.dat");
        n_gender_bin.open(gendb_dir + "/name/gender.bin");
        n_gdict_off.open(gendb_dir + "/name/gender.dict.off");
        n_gdict_dat.open(gendb_dir + "/name/gender.dict.dat");
        an_name_off.open(gendb_dir + "/aka_name/name.off");
        an_name_dat.open(gendb_dir + "/aka_name/name.dat");
        ci_movie_id.open(gendb_dir + "/cast_info/movie_id.bin");
        ml_link_type_id.open(gendb_dir + "/movie_link/link_type_id.bin");
        t_prod_year.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        pi_it_off.open(gendb_dir + "/_idx/person_info__info_type_id__offsets.bin");
        pi_it_rowids.open(gendb_dir + "/_idx/person_info__info_type_id__rowids.bin");
        an_off.open(gendb_dir + "/_idx/aka_name__person_id__offsets.bin");
        ci_p_off.open(gendb_dir + "/_idx/cast_info__person_id__offsets.bin");
        ci_p_rowids.open(gendb_dir + "/_idx/cast_info__person_id__rowids.bin");
        ml_lm_off.open(gendb_dir + "/_idx/movie_link__linked_movie_id__offsets.bin");
        ml_lm_rowids.open(gendb_dir + "/_idx/movie_link__linked_movie_id__rowids.bin");
    }

    // ---- Resolve dim codes ----
    int32_t target_it_id = -1;
    int32_t target_lt_id = -1;
    int8_t  code_m = 0;
    {
        GENDB_PHASE("resolve_dims");
        // info_type: 113 rows, info offsets are int64. id is dense 1..113.
        // it_info_off has 114 entries (N+1).
        const char* TARGET_IT = "mini biography";
        const size_t TARGET_IT_LEN = 14;
        size_t it_n = it_info_off.count - 1;
        for (size_t i = 0; i < it_n; ++i) {
            int64_t lo = it_info_off[i], hi = it_info_off[i+1];
            size_t len = (size_t)(hi - lo);
            if (len == TARGET_IT_LEN && std::memcmp(it_info_dat.data + lo, TARGET_IT, TARGET_IT_LEN) == 0) {
                target_it_id = (int32_t)(i + 1); // dense PK starting at 1
                break;
            }
        }
        // link_type: 18 rows, dense PK 1..18.
        const char* TARGET_LT = "features";
        const size_t TARGET_LT_LEN = 8;
        size_t lt_n = lt_link_off.count - 1;
        for (size_t i = 0; i < lt_n; ++i) {
            int64_t lo = lt_link_off[i], hi = lt_link_off[i+1];
            size_t len = (size_t)(hi - lo);
            if (len == TARGET_LT_LEN && std::memcmp(lt_link_dat.data + lo, TARGET_LT, TARGET_LT_LEN) == 0) {
                target_lt_id = (int32_t)(i + 1);
                break;
            }
        }
        // gender dict
        size_t g_n = n_gdict_off.count - 1;
        for (size_t i = 0; i < g_n; ++i) {
            int64_t lo = n_gdict_off[i], hi = n_gdict_off[i+1];
            size_t len = (size_t)(hi - lo);
            if (len == 1 && n_gdict_dat.data[lo] == 'm') {
                code_m = (int8_t)(i + 1);
                break;
            }
        }
    }

    if (target_it_id < 0 || target_lt_id < 0 || code_m == 0) {
        std::fprintf(stderr, "Dim resolution failed: target_it_id=%d target_lt_id=%d code_m=%d\n",
                     target_it_id, target_lt_id, (int)code_m);
        return 2;
    }

    // ---- Main scan: driver pi → name → aka → ci → title → ml ----
    std::string_view min_name;
    std::string_view min_title;
    bool has_min_name = false;
    bool has_min_title = false;

    auto cmp_lt = [](std::string_view a, std::string_view b) {
        // Lexicographic byte comparison (matches default SQL/LANG=C string ordering).
        size_t n = std::min(a.size(), b.size());
        int c = std::memcmp(a.data(), b.data(), n);
        if (c != 0) return c < 0;
        return a.size() < b.size();
    };

    {
        GENDB_PHASE("main_scan");
        const char* NOTE_VB = "Volker Boehm";
        const size_t NOTE_VB_LEN = 12;

        int32_t lo_pi = pi_it_off[target_it_id];
        int32_t hi_pi = pi_it_off[target_it_id + 1];

        for (int32_t j = lo_pi; j < hi_pi; ++j) {
            int32_t pi_row = pi_it_rowids[j];
            // check note
            int64_t no_lo = pi_note_off[pi_row], no_hi = pi_note_off[pi_row+1];
            if ((size_t)(no_hi - no_lo) != NOTE_VB_LEN) continue;
            if (std::memcmp(pi_note_dat.data + no_lo, NOTE_VB, NOTE_VB_LEN) != 0) continue;
            int32_t pid = pi_person_id[pi_row];
            if (pid <= 0) continue;
            int32_t pid_idx = pid - 1;

            // name.name_pcode_cf LIKE 'D%'
            int64_t pcf_lo = n_pcf_off[pid_idx], pcf_hi = n_pcf_off[pid_idx+1];
            if (pcf_hi <= pcf_lo) continue;
            if (n_pcf_dat.data[pcf_lo] != 'D') continue;
            // gender == 'm'
            if (n_gender_bin[pid_idx] != code_m) continue;

            // aka semi-join: any aka_name.name containing 'a'
            int32_t an_lo = an_off[pid], an_hi = an_off[pid + 1];
            bool aka_hit = false;
            for (int32_t r = an_lo; r < an_hi; ++r) {
                int64_t alo = an_name_off[r], ahi = an_name_off[r+1];
                size_t alen = (size_t)(ahi - alo);
                if (alen == 0) continue;
                if (std::memchr(an_name_dat.data + alo, 'a', alen) != nullptr) {
                    aka_hit = true;
                    break;
                }
            }
            if (!aka_hit) continue;

            // Get n.name view
            int64_t nn_lo = n_name_off[pid_idx], nn_hi = n_name_off[pid_idx+1];
            std::string_view cur_name(n_name_dat.data + nn_lo, (size_t)(nn_hi - nn_lo));

            // For each cast_info row → t_id → year filter → movie_link
            int32_t cilo = ci_p_off[pid], cihi = ci_p_off[pid + 1];
            for (int32_t k = cilo; k < cihi; ++k) {
                int32_t ci_row = ci_p_rowids[k];
                int32_t t_id = ci_movie_id[ci_row];
                if (t_id <= 0) continue;
                int32_t t_idx = t_id - 1;
                int32_t y = t_prod_year[t_idx];
                if (y == INT32_MIN) continue;
                if (y < 1980 || y > 1984) continue;

                // movie_link CSR by linked_movie_id == t_id, filter link_type_id == target_lt_id
                int32_t mlo = ml_lm_off[t_id], mhi = ml_lm_off[t_id + 1];
                bool ml_match = false;
                for (int32_t m = mlo; m < mhi; ++m) {
                    int32_t ml_row = ml_lm_rowids[m];
                    if (ml_link_type_id[ml_row] == target_lt_id) {
                        ml_match = true;
                        break;
                    }
                }
                if (!ml_match) continue;

                // Fold into MINs
                if (!has_min_name || cmp_lt(cur_name, min_name)) {
                    min_name = cur_name;
                    has_min_name = true;
                }
                int64_t tt_lo = t_title_off[t_idx], tt_hi = t_title_off[t_idx+1];
                std::string_view cur_title(t_title_dat.data + tt_lo, (size_t)(tt_hi - tt_lo));
                if (!has_min_title || cmp_lt(cur_title, min_title)) {
                    min_title = cur_title;
                    has_min_title = true;
                }
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q7b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "of_person,biography_movie\n");
        std::string name_out = has_min_name ? csv_quote(min_name) : std::string();
        std::string title_out = has_min_title ? csv_quote(min_title) : std::string();
        std::fprintf(f, "%s,%s\n", name_out.c_str(), title_out.c_str());
        std::fclose(f);
    }

    return 0;
}
