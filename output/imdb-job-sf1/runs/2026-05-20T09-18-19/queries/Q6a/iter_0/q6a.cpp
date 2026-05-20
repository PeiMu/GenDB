// Q6a — IMDB JOB
// SELECT MIN(k.keyword), MIN(n.name), MIN(t.title)
//   k.keyword='marvel-cinematic-universe' AND n.name LIKE '%Downey%Robert%' AND t.production_year>2010
//   joining cast_info, keyword, movie_keyword, name, title.
//
// Strategy (from plan):
//   1. Linear-scan keyword.dat/off for 'marvel-cinematic-universe' -> target_k_id
//   2. Parallel scan name.dat/off for memmem("Downey")<memmem("Robert") -> downey_robert_ids set
//   3. CSR enumerate mk rows for target_k_id -> t_ids
//   4. For each t_id, check production_year > 2010 (and != INT32_MIN); maintain min(title)
//   5. For each surviving t_id, scan ci range via offsets_only, test pid in set; maintain min(name)
//   6. Emit constant MIN(k.keyword), MIN(n.name), MIN(t.title)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <algorithm>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

static inline std::string_view varlen_at(const uint64_t* off, const char* dat, int32_t row) {
    uint64_t s = off[row], e = off[row + 1];
    return std::string_view(dat + s, e - s);
}

// memmem-style substring search returning position, or SIZE_MAX if not found.
static inline size_t find_substr(const char* s, size_t n, const char* pat, size_t plen) {
    if (plen == 0) return 0;
    if (n < plen) return SIZE_MAX;
    const void* p = memmem(s, n, pat, plen);
    if (!p) return SIZE_MAX;
    return (const char*)p - s;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [params...]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    std::filesystem::create_directories(results_dir);

    std::string min_keyword;
    std::string min_name;
    std::string min_title;
    bool have_result = false;

    {
        GENDB_PHASE("total");

        // ---- Data loading (mmap) ----
        MmapColumn<char>     kw_dat, name_dat, title_dat;
        MmapColumn<uint64_t> kw_off, name_off, title_off;
        MmapColumn<int32_t>  title_pyear;
        MmapColumn<int32_t>  mk_movie_id;
        MmapColumn<int32_t>  mk_kid_off, mk_kid_rowids;
        MmapColumn<int32_t>  ci_movie_off;
        MmapColumn<int32_t>  ci_person_id;

        {
            GENDB_PHASE("data_loading");
            kw_off.open(gendb_dir + "/keyword/keyword.off");
            kw_dat.open(gendb_dir + "/keyword/keyword.dat");
            name_off.open(gendb_dir + "/name/name.off");
            name_dat.open(gendb_dir + "/name/name.dat");
            title_off.open(gendb_dir + "/title/title.off");
            title_dat.open(gendb_dir + "/title/title.dat");
            title_pyear.open(gendb_dir + "/title/production_year.bin");
            mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
            mk_kid_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
            mk_kid_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
            ci_movie_off.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
            ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");
        }

        // ---- Resolve target_k_id ----
        int32_t target_k_id = -1;
        const char* needle_kw = "marvel-cinematic-universe";
        size_t needle_kw_len = std::strlen(needle_kw);
        {
            GENDB_PHASE("resolve_keyword");
            // keyword.off has N+1 entries
            size_t n_keywords = kw_off.count - 1;
            for (size_t i = 0; i < n_keywords; ++i) {
                uint64_t s = kw_off.data[i], e = kw_off.data[i + 1];
                if (e - s == needle_kw_len &&
                    std::memcmp(kw_dat.data + s, needle_kw, needle_kw_len) == 0) {
                    target_k_id = (int32_t)(i + 1); // dense PK
                    break;
                }
            }
        }

        if (target_k_id < 0) {
            // Emit single null-ish row? Per plan early-exit: emit NULL row.
            // We'll write header only with empty data row (commas).
            // Actually safer: leave have_result=false and produce nulls.
        }

        // ---- Build downey_robert_ids (parallel scan) ----
        std::unordered_set<int32_t> downey_robert_ids;
        downey_robert_ids.reserve(512);
        {
            GENDB_PHASE("name_filter_scan");
            size_t n_names = name_off.count - 1;
            unsigned T = std::thread::hardware_concurrency();
            if (T == 0) T = 4;
            if (T > 12) T = 12;

            std::vector<std::vector<int32_t>> local_ids(T);
            std::vector<std::thread> threads;
            threads.reserve(T);

            const char* nd = name_dat.data;
            const uint64_t* no = name_off.data;

            for (unsigned t = 0; t < T; ++t) {
                size_t lo = (n_names * t) / T;
                size_t hi = (n_names * (t + 1)) / T;
                threads.emplace_back([&, t, lo, hi]() {
                    auto& out = local_ids[t];
                    out.reserve(64);
                    const char* dn = "Downey";  size_t dnl = 6;
                    const char* rb = "Robert";  size_t rbl = 6;
                    for (size_t i = lo; i < hi; ++i) {
                        uint64_t s = no[i], e = no[i + 1];
                        size_t len = e - s;
                        const char* str = nd + s;
                        size_t pd = find_substr(str, len, dn, dnl);
                        if (pd == SIZE_MAX) continue;
                        // need 'Robert' after pos pd+dnl
                        if (pd + dnl > len) continue;
                        size_t pr = find_substr(str + pd + dnl, len - (pd + dnl), rb, rbl);
                        if (pr == SIZE_MAX) continue;
                        out.push_back((int32_t)(i + 1));
                    }
                });
            }
            for (auto& th : threads) th.join();
            for (auto& v : local_ids) {
                for (int32_t id : v) downey_robert_ids.insert(id);
            }
        }

        // ---- Driver: enumerate mk rows for target_k_id ----
        // Maintain min(title) over surviving t_ids; min(name) over matched person_ids
        std::string best_title;
        std::string best_name;
        bool have_title = false, have_name = false;

        if (target_k_id > 0 && !downey_robert_ids.empty()) {
            GENDB_PHASE("main_scan");
            int32_t kid_off_count = (int32_t)mk_kid_off.count; // 134172
            if (target_k_id + 1 < kid_off_count) {
                int32_t lo = mk_kid_off.data[target_k_id];
                int32_t hi = mk_kid_off.data[target_k_id + 1];

                for (int32_t j = lo; j < hi; ++j) {
                    int32_t mk_row = mk_kid_rowids.data[j];
                    int32_t t_id = mk_movie_id.data[mk_row];
                    if (t_id <= 0) continue;
                    int32_t py = title_pyear.data[t_id - 1];
                    if (py == INT32_MIN || py <= 2010) continue;

                    // Look up title for potential MIN update
                    std::string_view tv = varlen_at(title_off.data, title_dat.data, t_id - 1);

                    // Scan cast_info range for this t_id
                    int32_t cl = ci_movie_off.data[t_id];
                    int32_t ch = ci_movie_off.data[t_id + 1];
                    bool any_match_for_this_t = false;
                    for (int32_t r = cl; r < ch; ++r) {
                        int32_t pid = ci_person_id.data[r];
                        if (downey_robert_ids.count(pid)) {
                            // Matched: update min(name)
                            std::string_view nv = varlen_at(name_off.data, name_dat.data, pid - 1);
                            std::string ns(nv);
                            if (!have_name || ns < best_name) {
                                best_name = std::move(ns);
                                have_name = true;
                            }
                            any_match_for_this_t = true;
                        }
                    }

                    if (any_match_for_this_t) {
                        std::string ts(tv);
                        if (!have_title || ts < best_title) {
                            best_title = std::move(ts);
                            have_title = true;
                        }
                    }
                }
            }
        }

        if (have_name && have_title) {
            min_keyword = needle_kw; // MIN of single value
            min_name = best_name;
            min_title = best_title;
            have_result = true;
        }

        // ---- Output ----
        {
            GENDB_PHASE("output");
            std::string out_path = results_dir + "/Q6a.csv";
            FILE* f = std::fopen(out_path.c_str(), "w");
            if (!f) {
                std::fprintf(stderr, "Cannot open output %s\n", out_path.c_str());
                return 1;
            }
            std::fprintf(f, "movie_keyword,actor_name,marvel_movie\n");
            if (have_result) {
                // CSV quote when contains comma
                auto write_field = [&](const std::string& s) {
                    if (s.find(',') != std::string::npos || s.find('"') != std::string::npos) {
                        std::fputc('"', f);
                        for (char c : s) {
                            if (c == '"') std::fputc('"', f);
                            std::fputc(c, f);
                        }
                        std::fputc('"', f);
                    } else {
                        std::fwrite(s.data(), 1, s.size(), f);
                    }
                };
                write_field(min_keyword); std::fputc(',', f);
                write_field(min_name);    std::fputc(',', f);
                write_field(min_title);   std::fputc('\n', f);
            } else {
                std::fprintf(f, ",,\n");
            }
            std::fclose(f);
        }
    }

    return 0;
}
