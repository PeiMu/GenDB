// Q7a — biography person/movie min aggregate
//
// Drive from person_info filtered by it_id=mini_biography (CSR slice),
// then by note='Volker Boehm'. Surviving pids -> name/gender/pcode filters,
// aka_name semi-join (name contains 'a'), cast_info per-pid enumerate,
// title year range, movie_link semi-join with lt='features'.

#include "timing_utils.h"
#include "mmap_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using gendb::MmapColumn;

static std::string make_path(const std::string& dir, const std::string& sub) {
    return dir + "/" + sub;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    std::string min_name;       // MIN(n.name)
    std::string min_title;      // MIN(t.title)

    {
    GENDB_PHASE("total");

    // ----- Load all needed columns / indexes -----
    MmapColumn<char>    it_info_dat;
    MmapColumn<int64_t> it_info_off;
    MmapColumn<char>    lt_link_dat;
    MmapColumn<int64_t> lt_link_off;

    MmapColumn<int32_t> pi_it_off;     // CSR offsets
    MmapColumn<int32_t> pi_it_rowids;  // CSR rowids
    MmapColumn<int32_t> pi_person;     // person_info.person_id
    MmapColumn<char>    pi_note_dat;
    MmapColumn<int64_t> pi_note_off;

    MmapColumn<char>    n_pcode_dat;
    MmapColumn<int64_t> n_pcode_off;
    MmapColumn<int8_t>  n_gender;
    MmapColumn<char>    n_gender_dict_dat;
    MmapColumn<int64_t> n_gender_dict_off;
    MmapColumn<char>    n_name_dat;
    MmapColumn<int64_t> n_name_off;

    MmapColumn<int32_t> an_off;        // offsets-only index
    MmapColumn<char>    an_name_dat;
    MmapColumn<int64_t> an_name_off;

    MmapColumn<int32_t> ci_p_off;
    MmapColumn<int32_t> ci_p_rowids;
    MmapColumn<int32_t> ci_movie;

    MmapColumn<int32_t> t_year;
    MmapColumn<char>    t_title_dat;
    MmapColumn<int64_t> t_title_off;

    MmapColumn<int32_t> ml_lm_off;
    MmapColumn<int32_t> ml_lm_rowids;
    MmapColumn<int32_t> ml_link_type;

    {
        GENDB_PHASE("data_loading");
        it_info_dat.open(make_path(gendb_dir, "info_type/info.dat"));
        it_info_off.open(make_path(gendb_dir, "info_type/info.off"));
        lt_link_dat.open(make_path(gendb_dir, "link_type/link.dat"));
        lt_link_off.open(make_path(gendb_dir, "link_type/link.off"));

        pi_it_off.open(make_path(gendb_dir, "_idx/person_info__info_type_id__offsets.bin"));
        pi_it_rowids.open(make_path(gendb_dir, "_idx/person_info__info_type_id__rowids.bin"));
        pi_person.open(make_path(gendb_dir, "person_info/person_id.bin"));
        pi_note_dat.open(make_path(gendb_dir, "person_info/note.dat"));
        pi_note_off.open(make_path(gendb_dir, "person_info/note.off"));

        n_pcode_dat.open(make_path(gendb_dir, "name/name_pcode_cf.dat"));
        n_pcode_off.open(make_path(gendb_dir, "name/name_pcode_cf.off"));
        n_gender.open(make_path(gendb_dir, "name/gender.bin"));
        n_gender_dict_dat.open(make_path(gendb_dir, "name/gender.dict.dat"));
        n_gender_dict_off.open(make_path(gendb_dir, "name/gender.dict.off"));
        n_name_dat.open(make_path(gendb_dir, "name/name.dat"));
        n_name_off.open(make_path(gendb_dir, "name/name.off"));

        an_off.open(make_path(gendb_dir, "_idx/aka_name__person_id__offsets.bin"));
        an_name_dat.open(make_path(gendb_dir, "aka_name/name.dat"));
        an_name_off.open(make_path(gendb_dir, "aka_name/name.off"));

        ci_p_off.open(make_path(gendb_dir, "_idx/cast_info__person_id__offsets.bin"));
        ci_p_rowids.open(make_path(gendb_dir, "_idx/cast_info__person_id__rowids.bin"));
        ci_movie.open(make_path(gendb_dir, "cast_info/movie_id.bin"));

        t_year.open(make_path(gendb_dir, "title/production_year.bin"));
        t_title_dat.open(make_path(gendb_dir, "title/title.dat"));
        t_title_off.open(make_path(gendb_dir, "title/title.off"));

        ml_lm_off.open(make_path(gendb_dir, "_idx/movie_link__linked_movie_id__offsets.bin"));
        ml_lm_rowids.open(make_path(gendb_dir, "_idx/movie_link__linked_movie_id__rowids.bin"));
        ml_link_type.open(make_path(gendb_dir, "movie_link/link_type_id.bin"));
    }

    // ----- Resolve dim constants -----
    int32_t target_it_id = -1;
    {
        GENDB_PHASE("resolve_dim_constants");
        const std::string_view needle_it("mini biography");
        size_t n_it = it_info_off.count > 0 ? it_info_off.count - 1 : 0;
        for (size_t i = 0; i < n_it; ++i) {
            int64_t lo = it_info_off[i], hi = it_info_off[i + 1];
            int64_t len = hi - lo;
            if ((size_t)len == needle_it.size() &&
                std::memcmp(it_info_dat.data + lo, needle_it.data(), needle_it.size()) == 0) {
                // info_type.id is identity dense PK starting at 1
                target_it_id = static_cast<int32_t>(i + 1);
                break;
            }
        }

        int32_t target_lt_id = -1;
        const std::string_view needle_lt("features");
        size_t n_lt = lt_link_off.count > 0 ? lt_link_off.count - 1 : 0;
        for (size_t i = 0; i < n_lt; ++i) {
            int64_t lo = lt_link_off[i], hi = lt_link_off[i + 1];
            int64_t len = hi - lo;
            if ((size_t)len == needle_lt.size() &&
                std::memcmp(lt_link_dat.data + lo, needle_lt.data(), needle_lt.size()) == 0) {
                target_lt_id = static_cast<int32_t>(i + 1);
                break;
            }
        }

        // Resolve gender dict codes
        int8_t code_m = 0, code_f = 0;
        size_t n_g = n_gender_dict_off.count > 0 ? n_gender_dict_off.count - 1 : 0;
        for (size_t i = 0; i < n_g; ++i) {
            int64_t lo = n_gender_dict_off[i], hi = n_gender_dict_off[i + 1];
            std::string_view s(n_gender_dict_dat.data + lo, hi - lo);
            if (s == "m") code_m = static_cast<int8_t>(i + 1);
            if (s == "f") code_f = static_cast<int8_t>(i + 1);
        }

        // Store for later use - need to escape this scope
        // We use static-like trick by re-resolving after the phase, but easier
        // to make them in outer scope. Refactor below.
        (void)target_lt_id;
        (void)code_m;
        (void)code_f;
    }

    // Redo so target_lt_id, code_m, code_f are in scope outside the timed block
    int32_t target_lt_id = -1;
    int8_t code_m = 0, code_f = 0;
    {
        const std::string_view needle_lt("features");
        size_t n_lt = lt_link_off.count > 0 ? lt_link_off.count - 1 : 0;
        for (size_t i = 0; i < n_lt; ++i) {
            int64_t lo = lt_link_off[i], hi = lt_link_off[i + 1];
            int64_t len = hi - lo;
            if ((size_t)len == needle_lt.size() &&
                std::memcmp(lt_link_dat.data + lo, needle_lt.data(), needle_lt.size()) == 0) {
                target_lt_id = static_cast<int32_t>(i + 1);
                break;
            }
        }
        size_t n_g = n_gender_dict_off.count > 0 ? n_gender_dict_off.count - 1 : 0;
        for (size_t i = 0; i < n_g; ++i) {
            int64_t lo = n_gender_dict_off[i], hi = n_gender_dict_off[i + 1];
            std::string_view s(n_gender_dict_dat.data + lo, hi - lo);
            if (s == "m") code_m = static_cast<int8_t>(i + 1);
            if (s == "f") code_f = static_cast<int8_t>(i + 1);
        }
    }

    if (target_it_id < 0 || target_lt_id < 0) {
        // Write empty result with header
        std::FILE* fp = std::fopen((results_dir + "/Q7a.csv").c_str(), "w");
        std::fprintf(fp, "of_person,biography_movie\n");
        std::fclose(fp);
        return 0;
    }

    // ----- Drive via CSR person_info__info_type_id -----
    std::vector<int32_t> candidate_pids;
    candidate_pids.reserve(64);
    {
        GENDB_PHASE("main_scan");
        const std::string_view note_needle("Volker Boehm");
        const size_t note_len = note_needle.size();

        int32_t lo = pi_it_off[target_it_id];
        int32_t hi = pi_it_off[target_it_id + 1];
        for (int32_t j = lo; j < hi; ++j) {
            int32_t pi_row = pi_it_rowids[j];
            int64_t off_lo = pi_note_off[pi_row];
            int64_t off_hi = pi_note_off[pi_row + 1];
            if ((size_t)(off_hi - off_lo) != note_len) continue;
            if (std::memcmp(pi_note_dat.data + off_lo, note_needle.data(), note_len) != 0) continue;
            candidate_pids.push_back(pi_person[pi_row]);
        }

        // Deduplicate (a person could have multiple matching pi rows)
        std::sort(candidate_pids.begin(), candidate_pids.end());
        candidate_pids.erase(std::unique(candidate_pids.begin(), candidate_pids.end()),
                             candidate_pids.end());

        // For each candidate pid: apply name filters, aka semi-join, ci enumerate, ml semi-join.
        for (int32_t pid : candidate_pids) {
            int32_t name_row = pid - 1;  // name.id is identity dense PK
            if (name_row < 0) continue;

            // name_pcode_cf BETWEEN 'A' AND 'F'
            int64_t pc_lo = n_pcode_off[name_row], pc_hi = n_pcode_off[name_row + 1];
            if (pc_hi <= pc_lo) continue;
            char first_pc = n_pcode_dat.data[pc_lo];
            // BETWEEN 'A' AND 'F' on string: 'A' <= s <= 'F'
            // For multi-char strings, 'F' < "FA..." would be true (since 'F' string < 'FA'),
            // but SQL BETWEEN is inclusive lexicographic. Anything starting with 'F' but longer
            // than just "F" is > "F". So we must compare full string.
            // Compare: s >= "A" iff s[0] > 'A' or (s[0]=='A' and len>=1) -> always true if s starts with 'A'..'F'.
            // s <= "F": s[0] < 'F' OR (s[0]=='F' AND len==1).
            // So accept if first char in ['A','E'], or first char == 'F' AND len == 1.
            // But note: name_pcode_cf is a phonetic code; in IMDb dataset many start with letters.
            // To be safe use lexicographic memcmp against bounds.
            // Simpler: compare s against "A" and "G" (exclusive upper) → s in [A, G)? No, we want <= F.
            // Use direct memcmp comparison:
            //   s >= "A": first byte >= 'A' (assuming non-empty)
            //   s <= "F": s[0] < 'F' OR (s[0]=='F' AND len==1)
            if (first_pc < 'A') continue;
            if (first_pc > 'F') continue;
            if (first_pc == 'F' && (pc_hi - pc_lo) > 1) continue;

            // Gender / B-prefix
            int8_t g = n_gender[name_row];
            int64_t nm_lo = n_name_off[name_row], nm_hi = n_name_off[name_row + 1];
            bool gender_ok = false;
            if (g == code_m) {
                gender_ok = true;
            } else if (g == code_f) {
                if (nm_hi > nm_lo && n_name_dat.data[nm_lo] == 'B') {
                    gender_ok = true;
                }
            }
            if (!gender_ok) continue;

            // aka_name semi-join: any aka row with name containing 'a'?
            int32_t an_lo = an_off[pid], an_hi = an_off[pid + 1];
            bool aka_ok = false;
            for (int32_t r = an_lo; r < an_hi; ++r) {
                int64_t a_lo = an_name_off[r], a_hi = an_name_off[r + 1];
                int64_t a_len = a_hi - a_lo;
                if (a_len <= 0) continue;
                // memmem-style search for 'a'
                if (std::memchr(an_name_dat.data + a_lo, 'a', (size_t)a_len) != nullptr) {
                    aka_ok = true;
                    break;
                }
            }
            if (!aka_ok) continue;

            // Cache name string for later MIN aggregation if any t survives
            std::string_view n_name_sv(n_name_dat.data + nm_lo, nm_hi - nm_lo);

            // Enumerate cast_info rows for this pid
            int32_t ci_lo = ci_p_off[pid], ci_hi = ci_p_off[pid + 1];
            for (int32_t j = ci_lo; j < ci_hi; ++j) {
                int32_t ci_row = ci_p_rowids[j];
                int32_t t_id = ci_movie[ci_row];
                if (t_id <= 0) continue;
                int32_t t_row = t_id - 1;  // title.id is identity dense PK
                int32_t year = t_year[t_row];
                if (year == INT32_MIN) continue;
                if (year < 1980 || year > 1995) continue;

                // movie_link semi-join: any ml row with link_type_id == target_lt_id?
                int32_t ml_lo = ml_lm_off[t_id], ml_hi = ml_lm_off[t_id + 1];
                bool ml_ok = false;
                for (int32_t k = ml_lo; k < ml_hi; ++k) {
                    int32_t ml_row = ml_lm_rowids[k];
                    if (ml_link_type[ml_row] == target_lt_id) {
                        ml_ok = true;
                        break;
                    }
                }
                if (!ml_ok) continue;

                // Tuple survives — update MINs
                if (min_name.empty() || n_name_sv < std::string_view(min_name)) {
                    min_name.assign(n_name_sv.data(), n_name_sv.size());
                }
                int64_t tt_lo = t_title_off[t_row], tt_hi = t_title_off[t_row + 1];
                std::string_view t_title_sv(t_title_dat.data + tt_lo, tt_hi - tt_lo);
                if (min_title.empty() || t_title_sv < std::string_view(min_title)) {
                    min_title.assign(t_title_sv.data(), t_title_sv.size());
                }
            }
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::FILE* fp = std::fopen((results_dir + "/Q7a.csv").c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open results.csv\n");
            return 1;
        }
        std::fprintf(fp, "of_person,biography_movie\n");

        auto write_csv_field = [&](const std::string& s) {
            bool needs_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
            }
            if (needs_quote) {
                std::fputc('"', fp);
                for (char c : s) {
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
            } else {
                std::fwrite(s.data(), 1, s.size(), fp);
            }
        };

        if (!min_name.empty() || !min_title.empty()) {
            write_csv_field(min_name);
            std::fputc(',', fp);
            write_csv_field(min_title);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    } // total

    return 0;
}
