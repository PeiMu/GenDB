// Q17c: MIN(n.name) for X-prefix names in character-name-in-title movies w/ movie_companies
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <fstream>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

static std::string read_file_to_string(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    // Parameters (defaults match Q17c literals)
    std::string p_keyword = gendb::parse_string_arg(argc, argv, "--keyword", "character-name-in-title");
    std::string p_prefix  = gendb::parse_string_arg(argc, argv, "--name_prefix", "X");
    const char prefix_char = p_prefix.empty() ? 'X' : p_prefix[0];

    // ---- Resolve keyword id ----
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        MmapColumn<int64_t> kw_off(store + "/keyword/keyword.off");
        std::string kw_dat = read_file_to_string(store + "/keyword/keyword.dat");
        size_t n_kw = kw_off.size() - 1;
        size_t plen = p_keyword.size();
        const char* pdata = p_keyword.data();
        for (size_t i = 0; i < n_kw; i++) {
            int64_t a = kw_off[i], b = kw_off[i+1];
            if ((b - a) == (int64_t)plen && std::memcmp(kw_dat.data() + a, pdata, plen) == 0) {
                // keyword.id is 1-indexed (FK in movie_keyword uses id, not array position)
                k_id = (int32_t)(i + 1);
                break;
            }
        }
        if (k_id < 0) {
            std::fprintf(stderr, "keyword '%s' not found\n", p_keyword.c_str());
            return 2;
        }
    }

    // ---- Load name and build prefix bitset ----
    MmapColumn<int64_t> n_name_off(store + "/name/name.off");
    MmapColumn<char>    n_name_dat(store + "/name/name.dat");
    size_t n_name = n_name_off.size() - 1;
    std::vector<uint8_t> x_prefix(n_name, 0);
    {
        GENDB_PHASE("build_prefix_bitset");
        const int64_t* off = n_name_off.data;
        const char* dat = n_name_dat.data;
        // Parallelize over name range
        unsigned nth = std::thread::hardware_concurrency();
        if (nth == 0) nth = 4;
        std::vector<std::thread> threads;
        size_t chunk = (n_name + nth - 1) / nth;
        for (unsigned t = 0; t < nth; t++) {
            size_t lo = t * chunk;
            size_t hi = std::min(n_name, lo + chunk);
            if (lo >= hi) break;
            threads.emplace_back([&, lo, hi]() {
                for (size_t i = lo; i < hi; i++) {
                    int64_t a = off[i], b = off[i+1];
                    if ((b - a) >= 1 && dat[a] == prefix_char) {
                        x_prefix[i] = 1;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // ---- Open indexes and mk movie_id column ----
    MmapColumn<int32_t> mk_k_off, mk_k_row, mk_movie_id, mc_off, ci_off, ci_person_id;
    {
        GENDB_PHASE("data_loading");
        mk_k_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_k_row.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");
        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        ci_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_person_id.open(store + "/cast_info/person_id.bin");
    }

    // ---- Main scan over mk slice ----
    int32_t lo = mk_k_off[k_id];
    int32_t hi = mk_k_off[(size_t)k_id + 1];

    // Collect unique movie_ids from mk slice
    std::vector<int32_t> movie_ids;
    movie_ids.reserve(hi - lo);
    {
        GENDB_PHASE("collect_movie_ids");
        for (int32_t k = lo; k < hi; k++) {
            int32_t rid = mk_k_row[k];
            movie_ids.push_back(mk_movie_id[rid]);
        }
        std::sort(movie_ids.begin(), movie_ids.end());
        movie_ids.erase(std::unique(movie_ids.begin(), movie_ids.end()), movie_ids.end());
    }

    // ---- Per-thread MIN over movies ----
    std::string global_min;
    bool have_min = false;
    {
        GENDB_PHASE("main_scan");
        unsigned nth = std::thread::hardware_concurrency();
        if (nth == 0) nth = 4;
        size_t M = movie_ids.size();
        if (nth > M) nth = (unsigned)std::max((size_t)1, M);
        std::vector<std::string> local_min(nth);
        std::vector<uint8_t> local_have(nth, 0);
        const int64_t* n_off = n_name_off.data;
        const char* n_dat = n_name_dat.data;
        const int32_t* mc_off_p = mc_off.data;
        const int32_t* ci_off_p = ci_off.data;
        const int32_t* ci_pid = ci_person_id.data;
        size_t mc_off_count = mc_off.count;
        size_t ci_off_count = ci_off.count;

        std::vector<std::thread> threads;
        size_t chunk = (M + nth - 1) / nth;
        for (unsigned t = 0; t < nth; t++) {
            size_t lo_i = t * chunk;
            size_t hi_i = std::min(M, lo_i + chunk);
            if (lo_i >= hi_i) break;
            threads.emplace_back([&, t, lo_i, hi_i]() {
                std::string cur;
                bool have = false;
                for (size_t idx = lo_i; idx < hi_i; idx++) {
                    int32_t mv = movie_ids[idx];
                    if (mv < 0) continue;
                    size_t mv_u = (size_t)mv;
                    // mc existence
                    if (mv_u + 1 >= mc_off_count) continue;
                    int32_t mc_lo = mc_off_p[mv_u];
                    int32_t mc_hi = mc_off_p[mv_u + 1];
                    if (mc_lo == mc_hi) continue;
                    // ci probe
                    if (mv_u + 1 >= ci_off_count) continue;
                    int32_t ci_lo = ci_off_p[mv_u];
                    int32_t ci_hi = ci_off_p[mv_u + 1];
                    for (int32_t r = ci_lo; r < ci_hi; r++) {
                        int32_t pid = ci_pid[r];
                        int32_t name_idx = pid - 1;
                        if (name_idx < 0 || (size_t)name_idx >= n_name) continue;
                        if (!x_prefix[name_idx]) continue;
                        // compare name
                        int64_t a = n_off[name_idx];
                        int64_t b = n_off[name_idx + 1];
                        size_t nlen = (size_t)(b - a);
                        const char* s = n_dat + a;
                        if (!have) {
                            cur.assign(s, nlen);
                            have = true;
                        } else {
                            // lexicographic compare
                            size_t clen = cur.size();
                            size_t mlen = nlen < clen ? nlen : clen;
                            int cmp = std::memcmp(s, cur.data(), mlen);
                            if (cmp < 0 || (cmp == 0 && nlen < clen)) {
                                cur.assign(s, nlen);
                            }
                        }
                    }
                }
                local_min[t] = std::move(cur);
                local_have[t] = have ? 1 : 0;
            });
        }
        for (auto& th : threads) th.join();
        for (unsigned t = 0; t < threads.size(); t++) {
            if (!local_have[t]) continue;
            if (!have_min) {
                global_min = local_min[t];
                have_min = true;
            } else if (local_min[t] < global_min) {
                global_min = local_min[t];
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        ::mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q17c.csv";
        std::FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "member_in_charnamed_movie,a1\n");
        if (have_min) {
            // Quote with double quotes, escape inner quotes by doubling
            auto write_csv = [&](const std::string& s) {
                std::fputc('"', f);
                for (char c : s) {
                    if (c == '"') { std::fputc('"', f); std::fputc('"', f); }
                    else std::fputc(c, f);
                }
                std::fputc('"', f);
            };
            write_csv(global_min);
            std::fputc(',', f);
            write_csv(global_min);
            std::fputc('\n', f);
        } else {
            std::fprintf(f, ",\n");
        }
        std::fclose(f);
    }

    return 0;
}
