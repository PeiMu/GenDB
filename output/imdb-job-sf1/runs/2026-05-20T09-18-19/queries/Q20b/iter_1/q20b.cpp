// Q20b — MIN(t.title) for Robert Downey + Iron Man/Tony Stark (excluding Sherlock)
// + complete cast + 8 keyword set + title kind=movie + production_year > 2000.
//
// Strategy (per plan.json):
//   1. Resolve small dims: cct1_id ('cast'), cct2_ids (LIKE '%complete%'),
//      kt_movie (kind_type.kind='movie'), K_ids (8 keyword ids).
//   2. Scan name.name for '%Downey%Robert%' → N_ids (very few).
//   3. Scan char_name.name for (Tony+Stark OR Iron+Man) NOT Sherlock → CHN_bitset.
//   4. Drive cast_info via CSR(person_id) from N_ids; filter person_role_id ∈ CHN.
//   5. Collect candidate movie_ids (dedup'd).
//   6. For each candidate movie verify: title kind+year, complete_cast subject/status,
//      movie_keyword keyword_id ∈ K_ids. Aggregate MIN(title).
//
// I/O: mmap all column files (zero-copy).

#define _GNU_SOURCE
#include <cstring>
#include <string.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

// memmem-based substring search on string_view
static inline bool sv_contains(std::string_view hay, std::string_view ndl) {
    if (ndl.size() > hay.size()) return false;
    return memmem(hay.data(), hay.size(), ndl.data(), ndl.size()) != nullptr;
}

// Returns pointer to first occurrence of ndl in hay, or null.
static inline const char* sv_find(const char* hay, size_t hlen, const char* ndl, size_t nlen) {
    if (nlen > hlen) return nullptr;
    return (const char*) memmem(hay, hlen, ndl, nlen);
}

// Two-pointer LIKE '%a%b%': a then b after.
static inline bool sv_like_ab(std::string_view hay, std::string_view a, std::string_view b) {
    if (a.size() + b.size() > hay.size()) return false;
    const char* p = sv_find(hay.data(), hay.size(), a.data(), a.size());
    if (!p) return false;
    size_t off = (p - hay.data()) + a.size();
    return sv_find(hay.data() + off, hay.size() - off, b.data(), b.size()) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    // ----- Open mmap'd columns -----
    MmapColumn<int32_t>  title_id, title_kind_id, title_year;
    MmapColumn<int64_t>  title_off;
    MmapColumn<char>     title_dat;

    MmapColumn<int32_t>  kt_id;
    MmapColumn<int64_t>  kt_off;
    MmapColumn<char>     kt_dat;

    MmapColumn<int32_t>  cct_id;
    MmapColumn<int64_t>  cct_off;
    MmapColumn<char>     cct_dat;

    MmapColumn<int32_t>  kw_id;
    MmapColumn<int64_t>  kw_off;
    MmapColumn<char>     kw_dat;

    MmapColumn<int64_t>  name_off;
    MmapColumn<char>     name_dat;

    MmapColumn<int64_t>  chn_off;
    MmapColumn<char>     chn_dat;

    MmapColumn<int32_t>  ci_person_id, ci_movie_id, ci_person_role_id;

    MmapColumn<int32_t>  cc_subject_id, cc_status_id;
    MmapColumn<int32_t>  mk_keyword_id;

    MmapColumn<int32_t>  idx_ci_pid_off, idx_ci_pid_row;
    MmapColumn<int32_t>  idx_cc_mid_off, idx_mk_mid_off;

    {
        GENDB_PHASE("data_loading");
        title_id.open(store + "/title/id.bin");
        title_kind_id.open(store + "/title/kind_id.bin");
        title_year.open(store + "/title/production_year.bin");
        title_off.open(store + "/title/title.off");
        title_dat.open(store + "/title/title.dat");

        kt_id.open(store + "/kind_type/id.bin");
        kt_off.open(store + "/kind_type/kind.off");
        kt_dat.open(store + "/kind_type/kind.dat");

        cct_id.open(store + "/comp_cast_type/id.bin");
        cct_off.open(store + "/comp_cast_type/kind.off");
        cct_dat.open(store + "/comp_cast_type/kind.dat");

        kw_id.open(store + "/keyword/id.bin");
        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");

        name_off.open(store + "/name/name.off");
        name_dat.open(store + "/name/name.dat");

        chn_off.open(store + "/char_name/name.off");
        chn_dat.open(store + "/char_name/name.dat");

        ci_person_id.open(store + "/cast_info/person_id.bin");
        ci_movie_id.open(store + "/cast_info/movie_id.bin");
        ci_person_role_id.open(store + "/cast_info/person_role_id.bin");

        cc_subject_id.open(store + "/complete_cast/subject_id.bin");
        cc_status_id.open(store + "/complete_cast/status_id.bin");

        mk_keyword_id.open(store + "/movie_keyword/keyword_id.bin");

        idx_ci_pid_off.open(store + "/_idx/cast_info__person_id__offsets.bin");
        idx_ci_pid_row.open(store + "/_idx/cast_info__person_id__rowids.bin");
        idx_cc_mid_off.open(store + "/_idx/complete_cast__movie_id__offsets.bin");
        idx_mk_mid_off.open(store + "/_idx/movie_keyword__movie_id__offsets.bin");
    }

    // ----- Resolve small dims -----
    int32_t cct1_id = -1;
    std::vector<int32_t> cct2_ids;
    {
        GENDB_PHASE("resolve_cct");
        size_t n = cct_id.size();
        for (size_t i = 0; i < n; ++i) {
            int64_t s = cct_off[i], e = cct_off[i + 1];
            std::string_view k(cct_dat.data + s, e - s);
            int32_t id = cct_id[i];
            if (k == "cast") cct1_id = id;
            if (sv_contains(k, "complete")) cct2_ids.push_back(id);
        }
    }

    int32_t kt_movie = -1;
    {
        GENDB_PHASE("resolve_kt");
        size_t n = kt_id.size();
        for (size_t i = 0; i < n; ++i) {
            int64_t s = kt_off[i], e = kt_off[i + 1];
            std::string_view k(kt_dat.data + s, e - s);
            if (k == "movie") { kt_movie = kt_id[i]; break; }
        }
    }

    // K_ids — 8 keyword string literals
    std::unordered_set<int32_t> K_set;
    {
        GENDB_PHASE("resolve_keywords");
        static const char* WANTED[] = {
            "superhero", "sequel", "second-part", "marvel-comics",
            "based-on-comic", "tv-special", "fight", "violence"
        };
        const size_t W = sizeof(WANTED) / sizeof(WANTED[0]);
        size_t WLENS[8];
        for (size_t i = 0; i < W; ++i) WLENS[i] = std::strlen(WANTED[i]);
        size_t n = kw_id.size();
        for (size_t i = 0; i < n; ++i) {
            int64_t s = kw_off[i], e = kw_off[i + 1];
            size_t len = e - s;
            const char* p = kw_dat.data + s;
            for (size_t j = 0; j < W; ++j) {
                if (WLENS[j] == len && std::memcmp(p, WANTED[j], len) == 0) {
                    K_set.insert(kw_id[i]);
                    break;
                }
            }
        }
    }

    if (cct1_id < 0 || cct2_ids.empty() || kt_movie < 0 || K_set.empty()) {
        std::fprintf(stderr, "Dim resolution failed (cct1=%d cct2=%zu kt=%d k=%zu)\n",
                     cct1_id, cct2_ids.size(), kt_movie, K_set.size());
        return 1;
    }

    // ----- Filter name.name LIKE '%Downey%Robert%' → N_ids -----
    std::vector<int32_t> N_ids;
    {
        GENDB_PHASE("filter_name_downey_robert");
        size_t n = name_off.size() - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t s = name_off[i], e = name_off[i + 1];
            std::string_view nm(name_dat.data + s, e - s);
            if (sv_like_ab(nm, "Downey", "Robert")) {
                // id is dense, row i has id = i+1
                N_ids.push_back((int32_t)(i + 1));
            }
        }
    }

    // ----- Filter char_name.name → CHN bitset -----
    // (Tony+Stark OR Iron+Man) AND NOT Sherlock
    size_t chn_row_count = chn_off.size() - 1;
    std::vector<uint8_t> CHN_bs(chn_row_count + 2, 0);  // indexed by chn.id (dense, 1-based)
    {
        GENDB_PHASE("filter_char_name");
        for (size_t i = 0; i < chn_row_count; ++i) {
            int64_t s = chn_off[i], e = chn_off[i + 1];
            std::string_view nm(chn_dat.data + s, e - s);
            // Exclude Sherlock first (cheap cutoff for matches)
            bool match = sv_like_ab(nm, "Tony", "Stark") || sv_like_ab(nm, "Iron", "Man");
            if (!match) continue;
            if (sv_contains(nm, "Sherlock")) continue;
            CHN_bs[i + 1] = 1;
        }
    }

    // ----- CSR probe cast_info by person_id; filter person_role_id ∈ CHN; collect movies -----
    std::unordered_set<int32_t> cand_movies;
    {
        GENDB_PHASE("ci_probe");
        size_t off_count = idx_ci_pid_off.size();
        for (int32_t pid : N_ids) {
            if ((size_t)(pid + 1) >= off_count) continue;
            int32_t lo = idx_ci_pid_off[pid];
            int32_t hi = idx_ci_pid_off[pid + 1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = idx_ci_pid_row[k];
                int32_t prid = ci_person_role_id[r];
                if (prid <= 0 || (size_t)prid >= CHN_bs.size()) continue;
                if (!CHN_bs[prid]) continue;
                int32_t mv = ci_movie_id[r];
                cand_movies.insert(mv);
            }
        }
    }

    // ----- Verify each candidate movie -----
    std::string min_title;
    bool have_min = false;

    size_t title_rows = title_id.size();
    size_t cc_off_count = idx_cc_mid_off.size();
    size_t mk_off_count = idx_mk_mid_off.size();

    {
        GENDB_PHASE("main_scan");
        for (int32_t mv : cand_movies) {
            if (mv <= 0 || (size_t)mv > title_rows) continue;
            size_t tr = (size_t)(mv - 1);
            int32_t yr = title_year[tr];
            if (yr == INT32_MIN || yr <= 2000) continue;
            if (title_kind_id[tr] != kt_movie) continue;

            // complete_cast verify
            if ((size_t)(mv + 1) >= cc_off_count) continue;
            int32_t cc_lo = idx_cc_mid_off[mv];
            int32_t cc_hi = idx_cc_mid_off[mv + 1];
            bool cc_ok = false;
            for (int32_t r = cc_lo; r < cc_hi; ++r) {
                if (cc_subject_id[r] != cct1_id) continue;
                int32_t st = cc_status_id[r];
                for (int32_t s2 : cct2_ids) {
                    if (st == s2) { cc_ok = true; break; }
                }
                if (cc_ok) break;
            }
            if (!cc_ok) continue;

            // movie_keyword verify
            if ((size_t)(mv + 1) >= mk_off_count) continue;
            int32_t mk_lo = idx_mk_mid_off[mv];
            int32_t mk_hi = idx_mk_mid_off[mv + 1];
            bool mk_ok = false;
            for (int32_t r = mk_lo; r < mk_hi; ++r) {
                if (K_set.count(mk_keyword_id[r])) { mk_ok = true; break; }
            }
            if (!mk_ok) continue;

            // Aggregate MIN(title)
            int64_t ts = title_off[tr], te = title_off[tr + 1];
            std::string_view tv(title_dat.data + ts, te - ts);
            if (!have_min || tv < std::string_view(min_title)) {
                min_title.assign(tv.data(), tv.size());
                have_min = true;
            }
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q20b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "complete_downey_ironman_movie\n");
        if (have_min) {
            std::fwrite(min_title.data(), 1, min_title.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
