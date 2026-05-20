// Q18b: SELECT MIN(mi.info), MIN(mi_idx.info), MIN(t.title)
//       Joins: ci(note IN writer-set) -- n(gender='f') -- t(year 2008..2014)
//              -- mi(genres,Horror/Thriller,note NULL) -- mi_idx(rating > '8.0')
//
// Strategy:
//   Driver: title id range. For each movie, probe mi/mi_idx/ci offset ranges.
//   Per-thread MIN buffers; merge under critical at end.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// Bytewise lex compare: s > "8.0" ?
static inline bool gt_8_0(const char* s, size_t len) {
    static const char ref[] = "8.0";
    constexpr size_t ref_len = 3;
    size_t cmp_len = (len < ref_len) ? len : ref_len;
    int c = std::memcmp(s, ref, cmp_len);
    if (c != 0) return c > 0;
    return len > ref_len;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const std::string store = argv[1];
    const std::string out_dir = argv[2];
    std::filesystem::create_directories(out_dir);

    int32_t it_genres = -1, it_rating = -1;
    int8_t g_f = 0;

    // Storage handles
    MmapColumn<int32_t> t_py;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mi_it;
    MmapColumn<int64_t> mi_info_off, mi_note_off;
    MmapColumn<char>    mi_info_dat;

    MmapColumn<int32_t> mii_it;
    MmapColumn<int64_t> mii_info_off;
    MmapColumn<char>    mii_info_dat;

    MmapColumn<int32_t> ci_person;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;

    MmapColumn<int8_t>  n_gender;

    MmapColumn<int32_t> mi_off_idx, mii_off_idx, ci_off_idx;

    {
        GENDB_PHASE("data_loading");

        // --- Resolve info_type ids ---
        {
            MmapColumn<int64_t> it_off(store + "/info_type/info.off");
            MmapColumn<char>    it_dat(store + "/info_type/info.dat");
            MmapColumn<int32_t> it_id(store + "/info_type/id.bin");
            for (size_t i = 0; i + 1 < it_off.count; ++i) {
                int64_t lo = it_off[i], hi = it_off[i + 1];
                std::string_view s(it_dat.data + lo, (size_t)(hi - lo));
                if (s == "genres") it_genres = it_id[i];
                else if (s == "rating") it_rating = it_id[i];
            }
        }

        // --- Resolve g_f ---
        {
            MmapColumn<int64_t> g_off(store + "/name/gender.dict.off");
            MmapColumn<char>    g_dat(store + "/name/gender.dict.dat");
            for (size_t i = 0; i + 1 < g_off.count; ++i) {
                int64_t lo = g_off[i], hi = g_off[i + 1];
                std::string_view s(g_dat.data + lo, (size_t)(hi - lo));
                if (s == "f") { g_f = (int8_t)(i + 1); break; }
            }
        }

        if (it_genres < 0 || it_rating < 0 || g_f == 0) {
            std::fprintf(stderr, "Failed to resolve dict codes (it_genres=%d it_rating=%d g_f=%d)\n",
                         it_genres, it_rating, (int)g_f);
            return 2;
        }

        // --- mmap remaining columns ---
        t_py.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        mi_it.open(store + "/movie_info/info_type_id.bin");
        mi_info_off.open(store + "/movie_info/info.off");
        mi_info_dat.open(store + "/movie_info/info.dat");
        mi_note_off.open(store + "/movie_info/note.off");

        mii_it.open(store + "/movie_info_idx/info_type_id.bin");
        mii_info_off.open(store + "/movie_info_idx/info.off");
        mii_info_dat.open(store + "/movie_info_idx/info.dat");

        ci_person.open(store + "/cast_info/person_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        ci_note_dat.open(store + "/cast_info/note.dat");

        n_gender.open(store + "/name/gender.bin");
        n_gender.advise_random();

        mi_off_idx.open(store + "/_idx/movie_info__movie_id__offsets.bin");
        mii_off_idx.open(store + "/_idx/movie_info_idx__movie_id__offsets.bin");
        ci_off_idx.open(store + "/_idx/cast_info__movie_id__offsets.bin");

        mmap_prefetch_all(mi_off_idx, mii_off_idx, ci_off_idx);
    }

    std::string global_min_mi, global_min_mii, global_min_title;
    bool has_any = false;

    const int32_t n_titles = (int32_t)t_py.count;
    const int32_t it_genres_v = it_genres;
    const int32_t it_rating_v = it_rating;
    const int8_t  g_f_v       = g_f;

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel
        {
            std::string lmin_mi, lmin_mii, lmin_t;
            bool lhas = false;

            #pragma omp for schedule(dynamic, 8192) nowait
            for (int32_t mv = 1; mv <= n_titles; ++mv) {
                int32_t py = t_py.data[mv - 1];
                if (py < 2008 || py > 2014) continue;  // INT32_MIN excluded

                // ------ movie_info probe: it_genres, info IN {Horror,Thriller}, note IS NULL ------
                int32_t lo = mi_off_idx.data[mv];
                int32_t hi = mi_off_idx.data[mv + 1];
                std::string_view best_mi_info;
                bool found_mi = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mi_it.data[r] != it_genres_v) continue;
                    int64_t nlo = mi_note_off.data[r];
                    int64_t nhi = mi_note_off.data[r + 1];
                    if (nlo != nhi) continue;  // note IS NULL only
                    int64_t ilo = mi_info_off.data[r];
                    int64_t ihi = mi_info_off.data[r + 1];
                    size_t ilen = (size_t)(ihi - ilo);
                    const char* iptr = mi_info_dat.data + ilo;
                    bool match = (ilen == 6 && std::memcmp(iptr, "Horror",   6) == 0) ||
                                 (ilen == 8 && std::memcmp(iptr, "Thriller", 8) == 0);
                    if (!match) continue;
                    std::string_view sv(iptr, ilen);
                    if (!found_mi || sv < best_mi_info) { best_mi_info = sv; found_mi = true; }
                }
                if (!found_mi) continue;

                // ------ movie_info_idx probe: it_rating, info > '8.0' ------
                lo = mii_off_idx.data[mv];
                hi = mii_off_idx.data[mv + 1];
                std::string_view best_mii_info;
                bool found_mii = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mii_it.data[r] != it_rating_v) continue;
                    int64_t ilo = mii_info_off.data[r];
                    int64_t ihi = mii_info_off.data[r + 1];
                    size_t ilen = (size_t)(ihi - ilo);
                    const char* iptr = mii_info_dat.data + ilo;
                    if (!gt_8_0(iptr, ilen)) continue;
                    std::string_view sv(iptr, ilen);
                    if (!found_mii || sv < best_mii_info) { best_mii_info = sv; found_mii = true; }
                }
                if (!found_mii) continue;

                // ------ cast_info probe: note IN writer-set AND n.gender == 'f' ------
                lo = ci_off_idx.data[mv];
                hi = ci_off_idx.data[mv + 1];
                bool found_ci = false;
                for (int32_t r = lo; r < hi; ++r) {
                    int64_t nlo = ci_note_off.data[r];
                    int64_t nhi = ci_note_off.data[r + 1];
                    size_t nlen = (size_t)(nhi - nlo);
                    if (nlen != 7 && nlen != 8 && nlen != 12 && nlen != 13 && nlen != 14) continue;
                    const char* nptr = ci_note_dat.data + nlo;
                    bool note_match = false;
                    switch (nlen) {
                        case 7:  note_match = (std::memcmp(nptr, "(story)",        7)  == 0); break;
                        case 8:  note_match = (std::memcmp(nptr, "(writer)",       8)  == 0); break;
                        case 12: note_match = (std::memcmp(nptr, "(written by)",   12) == 0); break;
                        case 13: note_match = (std::memcmp(nptr, "(head writer)",  13) == 0); break;
                        case 14: note_match = (std::memcmp(nptr, "(story editor)", 14) == 0); break;
                    }
                    if (!note_match) continue;

                    int32_t pid = ci_person.data[r];
                    if (n_gender.data[pid - 1] == g_f_v) { found_ci = true; break; }
                }
                if (!found_ci) continue;

                // Title slice for this movie
                int64_t tlo = t_title_off.data[mv - 1];
                int64_t thi = t_title_off.data[mv];
                std::string_view title_sv(t_title_dat.data + tlo, (size_t)(thi - tlo));

                // Update local mins
                if (!lhas) {
                    lmin_mi.assign(best_mi_info.data(),  best_mi_info.size());
                    lmin_mii.assign(best_mii_info.data(), best_mii_info.size());
                    lmin_t.assign(title_sv.data(),       title_sv.size());
                    lhas = true;
                } else {
                    if (best_mi_info  < std::string_view(lmin_mi))
                        lmin_mi.assign(best_mi_info.data(),  best_mi_info.size());
                    if (best_mii_info < std::string_view(lmin_mii))
                        lmin_mii.assign(best_mii_info.data(), best_mii_info.size());
                    if (title_sv      < std::string_view(lmin_t))
                        lmin_t.assign(title_sv.data(),       title_sv.size());
                }
            }

            #pragma omp critical
            {
                if (lhas) {
                    if (!has_any) {
                        global_min_mi    = lmin_mi;
                        global_min_mii   = lmin_mii;
                        global_min_title = lmin_t;
                        has_any = true;
                    } else {
                        if (lmin_mi  < global_min_mi)    global_min_mi    = lmin_mi;
                        if (lmin_mii < global_min_mii)   global_min_mii   = lmin_mii;
                        if (lmin_t   < global_min_title) global_min_title = lmin_t;
                    }
                }
            }
        }
    }

    {
        GENDB_PHASE("output");
        std::string out_path = out_dir + "/Q18b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::perror("fopen"); return 3; }
        std::fprintf(f, "movie_budget,movie_votes,movie_title\n");
        if (has_any) {
            std::fprintf(f, "%s,%s,%s\n",
                         global_min_mi.c_str(),
                         global_min_mii.c_str(),
                         global_min_title.c_str());
        }
        std::fclose(f);
    }

    return 0;
}
