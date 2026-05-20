// Q6d: MIN(k.keyword), MIN(n.name), MIN(t.title)
// k.keyword IN (8 literals); n.name LIKE %Downey%Robert%; t.production_year > 2000
// Joins: k.id = mk.keyword_id, t.id = mk.movie_id = ci.movie_id, n.id = ci.person_id

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// LIKE '%Downey%Robert%': find "Downey" then "Robert" after it.
static inline bool like_downey_robert(const char* s, size_t len) {
    if (len < 12) return false;
    const void* p = memmem(s, len, "Downey", 6);
    if (!p) return false;
    const char* after = static_cast<const char*>(p) + 6;
    size_t remaining = len - (after - s);
    return memmem(after, remaining, "Robert", 6) != nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ----- Data loading (mmap) -----
    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;
    MmapColumn<int64_t> name_off;
    MmapColumn<char>    name_dat;
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;
    MmapColumn<int32_t> title_year;
    MmapColumn<int32_t> mk_kid_off;        // CSR offsets keyed by keyword_id
    MmapColumn<int32_t> mk_kid_rowids;
    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> ci_off_byT;
    MmapColumn<int32_t> ci_person;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");
        name_off.open(gendb_dir + "/name/name.off");
        name_dat.open(gendb_dir + "/name/name.dat");
        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");
        title_year.open(gendb_dir + "/title/production_year.bin");
        mk_kid_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kid_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
        ci_off_byT.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person.open(gendb_dir + "/cast_info/person_id.bin");
        ci_person.advise_random();
    }

    // ----- Build target keyword ids -----
    // IN ('superhero','sequel','second-part','marvel-comics','based-on-comic','tv-special','fight','violence')
    static const char* const TARGETS[8] = {
        "superhero", "sequel", "second-part", "marvel-comics",
        "based-on-comic", "tv-special", "fight", "violence"
    };
    static const size_t TARGET_LENS[8] = { 9, 6, 11, 13, 14, 10, 5, 8 };

    std::vector<int32_t> target_k_ids;
    std::vector<std::string> target_k_strs;
    target_k_ids.reserve(8);
    target_k_strs.reserve(8);

    {
        GENDB_PHASE("keyword_filter");
        size_t n_kw = kw_off.count - 1;
        const char* dat = kw_dat.data;
        const int64_t* off = kw_off.data;
        for (size_t i = 0; i < n_kw; ++i) {
            int64_t s = off[i];
            int64_t e = off[i + 1];
            size_t len = (size_t)(e - s);
            const char* p = dat + s;
            for (int t = 0; t < 8; ++t) {
                if (len == TARGET_LENS[t] && memcmp(p, TARGETS[t], len) == 0) {
                    int32_t k_id = (int32_t)(i + 1);
                    target_k_ids.push_back(k_id);
                    target_k_strs.emplace_back(p, len);
                    break;
                }
            }
            if (target_k_ids.size() == 8) break;
        }
    }

    // ----- Parallel name LIKE scan -----
    std::vector<int32_t> downey_ids_v;
    {
        GENDB_PHASE("name_filter");
        size_t n_name = name_off.count - 1;
        const char* ndat = name_dat.data;
        const int64_t* noff = name_off.data;

        int nthreads = omp_get_max_threads();
        std::vector<std::vector<int32_t>> per_thread(nthreads);
        for (auto& v : per_thread) v.reserve(64);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& local = per_thread[tid];
            #pragma omp for schedule(static)
            for (size_t i = 0; i < n_name; ++i) {
                int64_t s = noff[i];
                int64_t e = noff[i + 1];
                size_t len = (size_t)(e - s);
                if (like_downey_robert(ndat + s, len)) {
                    local.push_back((int32_t)(i + 1));
                }
            }
        }
        size_t total = 0;
        for (auto& v : per_thread) total += v.size();
        downey_ids_v.reserve(total);
        for (auto& v : per_thread) downey_ids_v.insert(downey_ids_v.end(), v.begin(), v.end());
    }

    std::unordered_set<int32_t> downey_ids;
    downey_ids.reserve(downey_ids_v.size() * 2 + 16);
    for (int32_t v : downey_ids_v) downey_ids.insert(v);

    // ----- CSR expand mk by keyword_id, dedupe t_ids (keep smallest k_keyword string) -----
    // map t_id -> index into target_k_strs (smallest lex k_keyword)
    std::unordered_map<int32_t, int> tid_to_kidx;
    tid_to_kidx.reserve(400000);

    {
        GENDB_PHASE("csr_expand");
        const int32_t* mk_off_p = mk_kid_off.data;
        const int32_t* mk_row_p = mk_kid_rowids.data;
        const int32_t* mk_mov_p = mk_movie_id.data;

        for (size_t ki = 0; ki < target_k_ids.size(); ++ki) {
            int32_t k_id = target_k_ids[ki];
            int32_t lo = mk_off_p[k_id];
            int32_t hi = mk_off_p[k_id + 1];
            const std::string& kw = target_k_strs[ki];
            for (int32_t j = lo; j < hi; ++j) {
                int32_t mk_row = mk_row_p[j];
                int32_t t_id = mk_mov_p[mk_row];
                auto it = tid_to_kidx.find(t_id);
                if (it == tid_to_kidx.end()) {
                    tid_to_kidx.emplace(t_id, (int)ki);
                } else {
                    if (kw < target_k_strs[it->second]) it->second = (int)ki;
                }
            }
        }
    }

    // ----- Year filter + cast_info semi-join -----
    // Update MIN aggregates on each accepted (t_id, n_id) pair.
    std::string min_keyword;
    std::string min_name;
    std::string min_title;
    bool any_match = false;

    {
        GENDB_PHASE("main_scan");
        const int32_t* year_p = title_year.data;
        const int32_t* ci_off_p = ci_off_byT.data;
        const int32_t* ci_pid_p = ci_person.data;
        const int64_t* noff = name_off.data;
        const char*    ndat = name_dat.data;
        const int64_t* toff = title_off.data;
        const char*    tdat = title_dat.data;

        for (auto& kv : tid_to_kidx) {
            int32_t t_id = kv.first;
            int kidx = kv.second;
            // bounds
            if (t_id <= 0) continue;
            int32_t y = year_p[t_id - 1];
            if (y == INT32_MIN || y <= 2000) continue;

            int32_t clo = ci_off_p[t_id];
            int32_t chi = ci_off_p[t_id + 1];
            bool t_matched = false;
            for (int32_t r = clo; r < chi; ++r) {
                int32_t pid = ci_pid_p[r];
                if (downey_ids.find(pid) == downey_ids.end()) continue;
                // accepted row: update MINs
                // name string for pid
                {
                    int64_t ns = noff[pid - 1];
                    int64_t ne = noff[pid];
                    std::string nm(ndat + ns, (size_t)(ne - ns));
                    if (!any_match || nm < min_name) min_name = std::move(nm);
                }
                t_matched = true;
            }
            if (t_matched) {
                // title for t_id
                int64_t ts = toff[t_id - 1];
                int64_t te = toff[t_id];
                std::string tt(tdat + ts, (size_t)(te - ts));
                const std::string& kw = target_k_strs[kidx];
                if (!any_match) {
                    min_title = std::move(tt);
                    min_keyword = kw;
                } else {
                    if (tt < min_title) min_title = std::move(tt);
                    if (kw < min_keyword) min_keyword = kw;
                }
                any_match = true;
            }
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q6d.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Failed to open %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "movie_keyword,actor_name,hero_movie\n");
        if (any_match) {
            // CSV-quote fields that contain comma or quote.
            auto write_field = [&](const std::string& s) {
                bool needs_q = s.find(',') != std::string::npos || s.find('"') != std::string::npos;
                if (!needs_q) {
                    std::fwrite(s.data(), 1, s.size(), f);
                } else {
                    std::fputc('"', f);
                    for (char c : s) {
                        if (c == '"') std::fputc('"', f);
                        std::fputc(c, f);
                    }
                    std::fputc('"', f);
                }
            };
            write_field(min_keyword);
            std::fputc(',', f);
            write_field(min_name);
            std::fputc(',', f);
            write_field(min_title);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
