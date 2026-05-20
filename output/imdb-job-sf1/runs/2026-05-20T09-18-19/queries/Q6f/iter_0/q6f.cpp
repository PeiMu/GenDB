// Q6f: hero/marvel/sequel keywords - no name filter
// SELECT MIN(k.keyword), MIN(n.name), MIN(t.title)
//   FROM cast_info ci, keyword k, movie_keyword mk, name n, title t
//  WHERE k.keyword IN ('superhero','sequel','second-part','marvel-comics',
//                      'based-on-comic','tv-special','fight','violence')
//    AND t.production_year > 2000
//    AND k.id = mk.keyword_id AND t.id = mk.movie_id
//    AND t.id = ci.movie_id AND n.id = ci.person_id;

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;
namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // ---------------------------------------------------------------
    // mmap all needed files
    // ---------------------------------------------------------------
    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;

    MmapColumn<int32_t> mk_off_idx;   // CSR offsets keyed by keyword_id
    MmapColumn<int32_t> mk_rowids;
    MmapColumn<int32_t> mk_movie_id;

    MmapColumn<int32_t> py_bin;       // title/production_year.bin
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;

    MmapColumn<int32_t> ci_off_idx;   // _idx/cast_info__movie_id__offsets.bin
    MmapColumn<int32_t> ci_person_id;

    MmapColumn<int64_t> name_off;
    MmapColumn<char>    name_dat;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        mk_off_idx.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");

        py_bin.open(gendb_dir + "/title/production_year.bin");
        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");

        ci_off_idx.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");

        name_off.open(gendb_dir + "/name/name.off");
        name_dat.open(gendb_dir + "/name/name.dat");

        // Prefetch big files (random-access patterns ahead)
        ci_person_id.prefetch();
        ci_off_idx.prefetch();
        name_off.prefetch();
        name_dat.prefetch();
        py_bin.prefetch();
    }

    // ---------------------------------------------------------------
    // Phase A1: Scan keyword to find target k_ids matching any of 8 literals
    // Compute MIN(k.keyword) inline.
    // ---------------------------------------------------------------
    std::vector<int32_t> target_k_ids;
    std::string min_keyword;
    {
        GENDB_PHASE("keyword_filter");
        // Sorted literal list (ASCII order). Provide also length for fast strcmp.
        static const char* const LITS[8] = {
            "based-on-comic", "fight", "marvel-comics", "second-part",
            "sequel", "superhero", "tv-special", "violence"
        };
        static const size_t LITS_LEN[8] = { 14, 5, 13, 11, 6, 9, 10, 8 };

        size_t n_keywords = kw_off.size() - 1; // 134170
        const int64_t* off = kw_off.data;
        const char* dat = kw_dat.data;
        target_k_ids.reserve(8);

        for (size_t i = 0; i < n_keywords; ++i) {
            int64_t lo = off[i], hi = off[i+1];
            size_t len = (size_t)(hi - lo);
            for (int li = 0; li < 8; ++li) {
                if (LITS_LEN[li] == len && std::memcmp(dat + lo, LITS[li], len) == 0) {
                    target_k_ids.push_back((int32_t)(i + 1)); // dense PK
                    std::string_view sv(dat + lo, len);
                    if (min_keyword.empty() || sv < min_keyword) {
                        min_keyword.assign(sv);
                    }
                    break;
                }
            }
            if ((int)target_k_ids.size() >= 8) break;
        }
    }

    // ---------------------------------------------------------------
    // Phase A2: For each k_id, probe CSR -> mk rows -> t_ids. Dedup via bitset.
    // Then filter by year > 2000.
    // ---------------------------------------------------------------
    static const size_t TITLE_N = 2528312;
    std::vector<uint64_t> tid_bitset((TITLE_N + 63) / 64, 0);
    std::vector<int32_t> qualifying_t_ids;
    std::string min_title;

    {
        GENDB_PHASE("csr_expand_and_year_filter");
        const int32_t* mk_off = mk_off_idx.data;
        const int32_t* mk_rid = mk_rowids.data;
        const int32_t* mk_mid = mk_movie_id.data;

        // dedup t_ids via bitset
        for (int32_t k_id : target_k_ids) {
            int32_t lo = mk_off[k_id], hi = mk_off[k_id + 1];
            for (int32_t j = lo; j < hi; ++j) {
                int32_t mk_row = mk_rid[j];
                int32_t t_id = mk_mid[mk_row];
                if (t_id >= 1 && (size_t)t_id <= TITLE_N) {
                    size_t idx = (size_t)(t_id - 1);
                    uint64_t mask = 1ULL << (idx & 63);
                    tid_bitset[idx >> 6] |= mask;
                }
            }
        }

        // iterate set bits, apply year filter, collect qualifying t_ids,
        // simultaneously compute MIN(t.title) over those.
        const int32_t* py = py_bin.data;
        const int64_t* t_off = title_off.data;
        const char* t_dat = title_dat.data;

        qualifying_t_ids.reserve(8192);
        size_t nw = tid_bitset.size();
        for (size_t w = 0; w < nw; ++w) {
            uint64_t bits = tid_bitset[w];
            while (bits) {
                int b = __builtin_ctzll(bits);
                bits &= bits - 1;
                size_t row = w * 64 + (size_t)b;
                int32_t t_id = (int32_t)(row + 1);
                int32_t year = py[row];
                if (year != INT32_MIN && year > 2000) {
                    qualifying_t_ids.push_back(t_id);
                    std::string_view tsv(t_dat + t_off[row],
                                         (size_t)(t_off[row + 1] - t_off[row]));
                    if (min_title.empty() || tsv < min_title) {
                        min_title.assign(tsv);
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Phase B: For each qualifying t_id, scan ci range, track MIN(n.name) over
    // unique person_ids (dedup via bitset). Parallel via OMP.
    // ---------------------------------------------------------------
    static const size_t NAME_N = 4167491;
    std::string min_name;

    {
        GENDB_PHASE("main_scan");
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int64_t* n_off  = name_off.data;
        const char*    n_dat  = name_dat.data;

        int num_threads = omp_get_max_threads();
        if (num_threads > 12) num_threads = 12;
        if (num_threads < 1) num_threads = 1;

        std::vector<std::string> per_thread_min(num_threads);

        size_t total = qualifying_t_ids.size();

        #pragma omp parallel num_threads(num_threads)
        {
            int tid = omp_get_thread_num();
            int nthr = omp_get_num_threads();
            // Each thread keeps a local seen-pid bitset to avoid duplicate
            // varlen reads. ~520KB per thread.
            std::vector<uint64_t> seen((NAME_N + 63) / 64, 0);
            std::string local_min;

            size_t start = (total * (size_t)tid) / (size_t)nthr;
            size_t end   = (total * (size_t)(tid + 1)) / (size_t)nthr;

            for (size_t i = start; i < end; ++i) {
                int32_t t_id = qualifying_t_ids[i];
                int32_t clo = ci_off[t_id];
                int32_t chi = ci_off[t_id + 1];
                for (int32_t r = clo; r < chi; ++r) {
                    int32_t pid = ci_pid[r];
                    if (pid < 1) continue;
                    size_t pidx = (size_t)(pid - 1);
                    if (pidx >= NAME_N) continue;
                    uint64_t mask = 1ULL << (pidx & 63);
                    if (seen[pidx >> 6] & mask) continue;
                    seen[pidx >> 6] |= mask;

                    std::string_view nsv(n_dat + n_off[pidx],
                                         (size_t)(n_off[pidx + 1] - n_off[pidx]));
                    if (local_min.empty() || nsv < local_min) {
                        local_min.assign(nsv);
                    }
                }
            }
            per_thread_min[tid] = std::move(local_min);
        }

        for (auto& s : per_thread_min) {
            if (s.empty()) continue;
            if (min_name.empty() || s < min_name) min_name = s;
        }
    }

    // ---------------------------------------------------------------
    // Output
    // ---------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q6f.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(fp, "movie_keyword,actor_name,hero_movie\n");
        if (!min_keyword.empty() && !min_name.empty() && !min_title.empty()) {
            auto quote_if_needed = [](const std::string& s) -> std::string {
                bool needs = false;
                for (char c : s) if (c == ',' || c == '"' || c == '\n') { needs = true; break; }
                if (!needs) return s;
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += "\"";
                return out;
            };
            std::fprintf(fp, "%s,%s,%s\n",
                         quote_if_needed(min_keyword).c_str(),
                         quote_if_needed(min_name).c_str(),
                         quote_if_needed(min_title).c_str());
        }
        std::fclose(fp);
    }

    return 0;
}
