// Q6c: marvel-cinematic-universe + Downey Robert + year>2014
// Strategy: pre-scan keyword.keyword to resolve target_k_id (sequential),
//           pre-scan name.name in parallel for '%Downey%Robert%',
//           CSR probe on movie_keyword__keyword_id, then per t_id check
//           production_year > 2014 and offsets_only probe on cast_info.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>
#include <filesystem>
#include <omp.h>

#include "mmap_utils.h"
#include "timing_utils.h"

using gendb::MmapColumn;

static inline bool contains(std::string_view hay, std::string_view needle, size_t from = 0) {
    return hay.find(needle, from) != std::string_view::npos;
}

static void write_csv_field(std::string& out, std::string_view s) {
    bool need_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        out.append(s.data(), s.size());
        return;
    }
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
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

    // ---- Data loading ----
    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;
    MmapColumn<int64_t> nm_off;
    MmapColumn<char>    nm_dat;
    MmapColumn<int64_t> ti_off;
    MmapColumn<char>    ti_dat;

    MmapColumn<int32_t> mk_off;
    MmapColumn<int32_t> mk_rowids;
    MmapColumn<int32_t> mk_movie_id;

    MmapColumn<int32_t> prod_year;

    MmapColumn<int32_t> ci_off;
    MmapColumn<int32_t> ci_person;

    {
        GENDB_PHASE("data_loading");
        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");

        nm_off.open(gendb_dir + "/name/name.off");
        nm_dat.open(gendb_dir + "/name/name.dat");

        ti_off.open(gendb_dir + "/title/title.off");
        ti_dat.open(gendb_dir + "/title/title.dat");

        mk_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");

        prod_year.open(gendb_dir + "/title/production_year.bin");

        ci_off.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person.open(gendb_dir + "/cast_info/person_id.bin");
    }

    // ---- Resolve target_k_id ----
    static const std::string_view TARGET_KW = "marvel-cinematic-universe";
    int32_t target_k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        size_t n = kw_off.count - 1; // 134170 rows
        const int64_t* off = kw_off.data;
        const char* dat = kw_dat.data;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = off[i], hi = off[i+1];
            size_t len = (size_t)(hi - lo);
            if (len != TARGET_KW.size()) continue;
            if (std::memcmp(dat + lo, TARGET_KW.data(), len) == 0) {
                // keyword has dense PK starting at id=1; row i -> id i+1
                target_k_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    // If no keyword match, emit three NULLs and exit.
    if (target_k_id < 0) {
        std::string path = results_dir + "/Q6c.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        std::fprintf(f, "movie_keyword,actor_name,marvel_movie\n,,\n");
        std::fclose(f);
        return 0;
    }

    // ---- Build downey_robert_ids by parallel pre-scan over name.name ----
    std::unordered_set<int32_t> downey_robert_ids;
    {
        GENDB_PHASE("name_filter");
        size_t n = nm_off.count - 1;
        const int64_t* off = nm_off.data;
        const char* dat = nm_dat.data;

        int nthreads = omp_get_max_threads();
        std::vector<std::vector<int32_t>> per_thread(nthreads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& local = per_thread[tid];
            local.reserve(64);
            #pragma omp for schedule(static)
            for (long long i = 0; i < (long long)n; ++i) {
                int64_t lo = off[i], hi = off[i+1];
                size_t len = (size_t)(hi - lo);
                if (len < 13) continue; // "Downey"+"Robert" = 12 chars minimum
                std::string_view s(dat + lo, len);
                size_t p1 = s.find("Downey");
                if (p1 == std::string_view::npos) continue;
                size_t p2 = s.find("Robert", p1 + 6);
                if (p2 == std::string_view::npos) continue;
                // name has dense PK starting id=1
                local.push_back((int32_t)(i + 1));
            }
        }
        size_t total = 0;
        for (auto& v : per_thread) total += v.size();
        downey_robert_ids.reserve(total * 2 + 16);
        for (auto& v : per_thread) for (int32_t x : v) downey_robert_ids.insert(x);
    }

    // ---- Main scan: CSR slice over movie_keyword for target_k_id ----
    // Track MINs lazily; only fetch varlen on potential update.
    std::string min_name;
    std::string min_title;
    bool have_name = false, have_title = false;
    bool any_match = false;

    {
        GENDB_PHASE("main_scan");
        int32_t lo_mk = mk_off.data[target_k_id];
        int32_t hi_mk = mk_off.data[target_k_id + 1];

        const int32_t* mk_rid = mk_rowids.data;
        const int32_t* mk_mid = mk_movie_id.data;
        const int32_t* py     = prod_year.data;
        const int32_t* cio    = ci_off.data;
        const int32_t* cip    = ci_person.data;
        const int64_t* nof    = nm_off.data;
        const char*    ndt    = nm_dat.data;
        const int64_t* tof    = ti_off.data;
        const char*    tdt    = ti_dat.data;

        for (int32_t j = lo_mk; j < hi_mk; ++j) {
            int32_t mk_row = mk_rid[j];
            int32_t t_id   = mk_mid[mk_row];
            int32_t year   = py[t_id - 1];
            if (year == INT32_MIN || year <= 2014) continue;

            int32_t lo_ci = cio[t_id];
            int32_t hi_ci = cio[t_id + 1];
            // Walk ci slice
            for (int32_t r = lo_ci; r < hi_ci; ++r) {
                int32_t pid = cip[r];
                if (downey_robert_ids.find(pid) == downey_robert_ids.end()) continue;

                any_match = true;

                // Compare name (n.id == pid; row = pid - 1)
                int64_t nlo = nof[pid - 1], nhi = nof[pid];
                std::string_view nv(ndt + nlo, (size_t)(nhi - nlo));
                if (!have_name || nv < std::string_view(min_name)) {
                    min_name.assign(nv);
                    have_name = true;
                }
                // Compare title (t.id == t_id; row = t_id - 1)
                int64_t tlo = tof[t_id - 1], thi = tof[t_id];
                std::string_view tv(tdt + tlo, (size_t)(thi - tlo));
                if (!have_title || tv < std::string_view(min_title)) {
                    min_title.assign(tv);
                    have_title = true;
                }
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string path = results_dir + "/Q6c.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        std::fprintf(f, "movie_keyword,actor_name,marvel_movie\n");

        std::string row;
        if (any_match) {
            write_csv_field(row, TARGET_KW);
            row.push_back(',');
            write_csv_field(row, min_name);
            row.push_back(',');
            write_csv_field(row, min_title);
        } else {
            row = ",,";
        }
        row.push_back('\n');
        std::fwrite(row.data(), 1, row.size(), f);
        std::fclose(f);
    }

    return 0;
}
