// Q16c — IMDB JOB
//   SELECT MIN(an.name), MIN(t.title)
//   FROM aka_name an, cast_info ci, company_name cn, keyword k,
//        movie_companies mc, movie_keyword mk, name n, title t
//   WHERE cn.country_code = '[us]' AND k.keyword = 'character-name-in-title'
//     AND t.episode_nr < 100 AND ...
//
// Strategy (from plan):
//   1. Resolve keyword_id by scanning keyword/keyword text column.
//   2. Resolve dict code for '[us]' from company_name/country_code.dict.dat.
//   3. Build us_company bitset over company_name.id.
//   4. Enumerate distinct movie_ids for keyword_id via movie_keyword aux CSR.
//   5. For each candidate movie_id v in parallel:
//        - filter title.episode_nr[v-1] < upper && >= 0
//        - movie_companies semi-join: at least one company in us bitset
//        - cast_info -> person_id -> aka_name primary CSR; if any person has
//          aka_name entries, the movie qualifies. Update min(an.name) over
//          all matching aka_name rows.
//        - Update min(t.title) once movie qualifies.
//   6. Reduce thread-local mins, write CSV.

#include "timing_utils.h"
#include "cli_params.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <omp.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace gendb;

static const char* mmap_file(const std::string& path, size_t* out_size) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::fprintf(stderr, "fstat failed for %s\n", path.c_str());
        std::exit(1);
    }
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed for %s\n", path.c_str());
        std::exit(1);
    }
    ::close(fd);
    if (st.st_size > 4 * 1024 * 1024) {
        madvise(p, st.st_size, MADV_WILLNEED);
    }
    if (out_size) *out_size = st.st_size;
    return reinterpret_cast<const char*>(p);
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--param value ...]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    int64_t episode_nr_upper = parse_int_arg(argc, argv, "--episode_nr_upper", 100);
    std::string country_code_eq = parse_string_arg(argc, argv, "--country_code_eq", "[us]");
    std::string keyword_eq      = parse_string_arg(argc, argv, "--keyword_eq", "character-name-in-title");

    size_t sz;

    // ----- data loading -----
    const char*     kw_dat;
    const uint64_t* kw_off;
    const int32_t*  kw_id;
    size_t          kw_rows;

    const char*     cc_dict_dat;
    const uint64_t* cc_dict_off;
    size_t          cc_dict_entries;
    const int16_t*  cn_country_code;
    const int32_t*  cn_id;
    size_t          cn_rows;

    const int32_t* mk_kid_off;
    size_t         mk_kid_off_count;
    const int32_t* mk_kid_rowids;
    const int32_t* mk_movie_id;

    const int32_t* mc_mid_off;
    size_t         mc_mid_off_count;
    const int32_t* mc_company_id;

    const int32_t* ci_mid_off;
    size_t         ci_mid_off_count;
    const int32_t* ci_person_id;

    const int32_t* an_pid_off;
    size_t         an_pid_off_count;
    const uint64_t* an_name_off;
    const char*     an_name_dat;

    const int32_t*  t_episode_nr;
    size_t          t_rows;
    const uint64_t* t_title_off;
    const char*     t_title_dat;

    {
        GENDB_PHASE("data_loading");

        kw_dat  = mmap_file(gendb_dir + "/keyword/keyword.dat", &sz);
        kw_off  = (const uint64_t*) mmap_file(gendb_dir + "/keyword/keyword.off", &sz);
        kw_id   = (const int32_t*)  mmap_file(gendb_dir + "/keyword/id.bin", &sz);
        kw_rows = sz / sizeof(int32_t);

        cc_dict_dat     = mmap_file(gendb_dir + "/company_name/country_code.dict.dat", &sz);
        cc_dict_off     = (const uint64_t*) mmap_file(gendb_dir + "/company_name/country_code.dict.off", &sz);
        cc_dict_entries = (sz / sizeof(uint64_t)) - 1;
        cn_country_code = (const int16_t*) mmap_file(gendb_dir + "/company_name/country_code.bin", &sz);
        cn_id           = (const int32_t*) mmap_file(gendb_dir + "/company_name/id.bin", &sz);
        cn_rows         = sz / sizeof(int32_t);

        mk_kid_off       = (const int32_t*) mmap_file(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin", &sz);
        mk_kid_off_count = sz / sizeof(int32_t);
        mk_kid_rowids    = (const int32_t*) mmap_file(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin", &sz);
        mk_movie_id      = (const int32_t*) mmap_file(gendb_dir + "/movie_keyword/movie_id.bin", &sz);

        mc_mid_off       = (const int32_t*) mmap_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin", &sz);
        mc_mid_off_count = sz / sizeof(int32_t);
        mc_company_id    = (const int32_t*) mmap_file(gendb_dir + "/movie_companies/company_id.bin", &sz);

        ci_mid_off       = (const int32_t*) mmap_file(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin", &sz);
        ci_mid_off_count = sz / sizeof(int32_t);
        ci_person_id     = (const int32_t*) mmap_file(gendb_dir + "/cast_info/person_id.bin", &sz);

        an_pid_off       = (const int32_t*) mmap_file(gendb_dir + "/_idx/aka_name__person_id__offsets.bin", &sz);
        an_pid_off_count = sz / sizeof(int32_t);
        an_name_off      = (const uint64_t*) mmap_file(gendb_dir + "/aka_name/name.off", &sz);
        an_name_dat      = mmap_file(gendb_dir + "/aka_name/name.dat", &sz);

        t_episode_nr = (const int32_t*) mmap_file(gendb_dir + "/title/episode_nr.bin", &sz);
        t_rows       = sz / sizeof(int32_t);
        t_title_off  = (const uint64_t*) mmap_file(gendb_dir + "/title/title.off", &sz);
        t_title_dat  = mmap_file(gendb_dir + "/title/title.dat", &sz);
    }

    // ----- resolve keyword literal -----
    int32_t keyword_id = -1;
    {
        for (size_t r = 0; r < kw_rows; ++r) {
            uint64_t lo = kw_off[r], hi = kw_off[r + 1];
            std::string_view s(kw_dat + lo, hi - lo);
            if (s == keyword_eq) { keyword_id = kw_id[r]; break; }
        }
    }

    // ----- resolve country_code dict code -----
    int16_t us_code = 0; // 0 = NULL / not found
    {
        for (size_t r = 0; r < cc_dict_entries; ++r) {
            uint64_t lo = cc_dict_off[r], hi = cc_dict_off[r + 1];
            std::string_view s(cc_dict_dat + lo, hi - lo);
            if (s == country_code_eq) {
                us_code = (int16_t)(r + 1); // dict codes start at 1
                break;
            }
        }
    }

    // ----- build us_company bitset over company_name.id -----
    std::vector<uint64_t> us_bits;
    int32_t max_company_id = 0;
    {
        GENDB_PHASE("build_us_company_bitset");
        for (size_t r = 0; r < cn_rows; ++r) {
            int32_t id = cn_id[r];
            if (id > max_company_id) max_company_id = id;
        }
        us_bits.assign((size_t)(max_company_id / 64) + 2, 0);
        if (us_code != 0) {
            for (size_t r = 0; r < cn_rows; ++r) {
                if (cn_country_code[r] == us_code) {
                    int32_t id = cn_id[r];
                    if (id > 0) {
                        us_bits[(size_t)(id >> 6)] |= (1ULL << (id & 63));
                    }
                }
            }
        }
    }

    // ----- enumerate distinct movie_ids for the keyword -----
    std::vector<int32_t> candidate_movies;
    {
        GENDB_PHASE("enumerate_movies");
        if (keyword_id >= 0 && (size_t)keyword_id + 1 < mk_kid_off_count) {
            int32_t lo = mk_kid_off[keyword_id];
            int32_t hi = mk_kid_off[keyword_id + 1];
            candidate_movies.reserve((size_t)(hi - lo));
            for (int32_t i = lo; i < hi; ++i) {
                int32_t row = mk_kid_rowids[i];
                candidate_movies.push_back(mk_movie_id[row]);
            }
            std::sort(candidate_movies.begin(), candidate_movies.end());
            candidate_movies.erase(std::unique(candidate_movies.begin(), candidate_movies.end()),
                                    candidate_movies.end());
        }
        std::fprintf(stderr, "[INFO] candidate movies for keyword: %zu\n", candidate_movies.size());
    }

    int num_threads = std::max(1, std::min(12, (int)std::thread::hardware_concurrency()));
    std::vector<std::string> thr_min_name(num_threads);
    std::vector<std::string> thr_min_title(num_threads);
    std::vector<char> thr_has_name(num_threads, 0);
    std::vector<char> thr_has_title(num_threads, 0);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel num_threads(num_threads)
        {
            int tid = omp_get_thread_num();
            std::string local_min_name;
            std::string local_min_title;
            bool has_name = false, has_title = false;

            #pragma omp for schedule(static)
            for (size_t idx = 0; idx < candidate_movies.size(); ++idx) {
                int32_t v = candidate_movies[idx];
                if (v <= 0 || (size_t)v > t_rows) continue;
                // title PK is dense 1..N => pos = v - 1
                int32_t pos = v - 1;
                int32_t enr = t_episode_nr[pos];
                if (enr < 0 || enr >= (int32_t)episode_nr_upper) continue;

                // movie_companies semi-join: at least one US company
                bool has_us = false;
                if ((size_t)v + 1 < mc_mid_off_count) {
                    int32_t mlo = mc_mid_off[v];
                    int32_t mhi = mc_mid_off[v + 1];
                    for (int32_t r = mlo; r < mhi; ++r) {
                        int32_t cid = mc_company_id[r];
                        if (cid > 0 && cid <= max_company_id) {
                            if (us_bits[(size_t)(cid >> 6)] & (1ULL << (cid & 63))) {
                                has_us = true;
                                break;
                            }
                        }
                    }
                }
                if (!has_us) continue;

                // cast_info -> person_id -> aka_name
                bool movie_qualifies = false;
                if ((size_t)v + 1 < ci_mid_off_count) {
                    int32_t cilo = ci_mid_off[v];
                    int32_t cihi = ci_mid_off[v + 1];
                    for (int32_t r = cilo; r < cihi; ++r) {
                        int32_t pid = ci_person_id[r];
                        if (pid < 0 || (size_t)pid + 1 >= an_pid_off_count) continue;
                        int32_t alo = an_pid_off[pid];
                        int32_t ahi = an_pid_off[pid + 1];
                        if (alo >= ahi) continue;
                        movie_qualifies = true;
                        for (int32_t ar = alo; ar < ahi; ++ar) {
                            uint64_t nlo = an_name_off[ar];
                            uint64_t nhi = an_name_off[ar + 1];
                            if (nlo == nhi) continue; // NULL
                            std::string_view s(an_name_dat + nlo, nhi - nlo);
                            if (!has_name ||
                                s < std::string_view(local_min_name.data(), local_min_name.size())) {
                                local_min_name.assign(s);
                                has_name = true;
                            }
                        }
                    }
                }
                if (!movie_qualifies) continue;

                // Update min title for this movie
                uint64_t tlo = t_title_off[pos];
                uint64_t thi = t_title_off[pos + 1];
                if (tlo < thi) {
                    std::string_view ts(t_title_dat + tlo, thi - tlo);
                    if (!has_title ||
                        ts < std::string_view(local_min_title.data(), local_min_title.size())) {
                        local_min_title.assign(ts);
                        has_title = true;
                    }
                }
            }

            thr_min_name[tid]  = std::move(local_min_name);
            thr_min_title[tid] = std::move(local_min_title);
            thr_has_name[tid]  = has_name ? 1 : 0;
            thr_has_title[tid] = has_title ? 1 : 0;
        }
    }

    std::string global_min_name;
    std::string global_min_title;
    bool have_min_name = false, have_min_title = false;
    for (int t = 0; t < num_threads; ++t) {
        if (thr_has_name[t]) {
            if (!have_min_name || thr_min_name[t] < global_min_name) {
                global_min_name = thr_min_name[t];
                have_min_name = true;
            }
        }
        if (thr_has_title[t]) {
            if (!have_min_title || thr_min_title[t] < global_min_title) {
                global_min_title = thr_min_title[t];
                have_min_title = true;
            }
        }
    }

    // ----- output -----
    {
        GENDB_PHASE("output");
        std::error_code ec;
        std::filesystem::create_directories(results_dir, ec);
        std::string out_path = results_dir + "/Q16c.csv";
        std::ofstream out(out_path);
        out << "cool_actor_pseudonym,series_named_after_char\n";

        auto escape_csv = [](const std::string& s) -> std::string {
            bool need_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                    need_quote = true; break;
                }
            }
            if (!need_quote) return s;
            std::string r;
            r.reserve(s.size() + 2);
            r.push_back('"');
            for (char c : s) {
                if (c == '"') { r.push_back('"'); r.push_back('"'); }
                else r.push_back(c);
            }
            r.push_back('"');
            return r;
        };

        out << (have_min_name ? escape_csv(global_min_name) : "")
            << ","
            << (have_min_title ? escape_csv(global_min_title) : "")
            << "\n";
    }

    return 0;
}
