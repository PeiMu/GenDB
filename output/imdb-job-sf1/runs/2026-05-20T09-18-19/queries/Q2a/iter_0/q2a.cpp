// Q2a: SELECT MIN(t.title) FROM cn, k, mc, mk, t
//   WHERE cn.country_code='[de]' AND k.keyword='character-name-in-title'
//     AND cn.id = mc.company_id AND mc.movie_id = t.id
//     AND t.id = mk.movie_id AND mk.keyword_id = k.id;

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    // ---------- mmap all columns/indexes ----------
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int64_t> cn_cc_dict_off;
    MmapColumn<char>    cn_cc_dict_dat;

    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;

    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mk_kw_idx_off;
    MmapColumn<int32_t> mk_kw_idx_rowids;

    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_mv_idx_off;

    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;

    {
        GENDB_PHASE("data_loading");
        cn_country_code.open(store + "/company_name/country_code.bin");
        cn_cc_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(store + "/company_name/country_code.dict.dat");

        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");

        mk_keyword_id.open(store + "/movie_keyword/keyword_id.bin");
        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");
        mk_kw_idx_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_idx_rowids.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");

        mc_movie_id.open(store + "/movie_companies/movie_id.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");
        mc_mv_idx_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");

        title_off.open(store + "/title/title.off");
        title_dat.open(store + "/title/title.dat");

        // random-access patterns
        mc_company_id.advise_random();
        mc_movie_id.advise_random();
        mk_kw_idx_rowids.advise_random();
        mk_movie_id.advise_random();
        title_off.advise_random();
        title_dat.advise_random();
    }

    // ---------- runtime literal resolution ----------
    int16_t target_code = 0;
    {
        GENDB_PHASE("resolve_target_code");
        size_t n_entries = cn_cc_dict_off.count;
        // n_entries is number of offsets; entries = n_entries - 1
        for (size_t i = 0; i + 1 < n_entries; ++i) {
            int64_t lo = cn_cc_dict_off[i];
            int64_t hi = cn_cc_dict_off[i + 1];
            std::string_view s(cn_cc_dict_dat.data + lo, (size_t)(hi - lo));
            if (s == "[de]") { target_code = (int16_t)(i + 1); break; }
        }
        if (target_code == 0) {
            std::fprintf(stderr, "Could not resolve '[de]'\n");
            return 2;
        }
    }

    int32_t target_k_id = 0;
    {
        GENDB_PHASE("resolve_target_k_id");
        size_t n_kw_off = kw_off.count;
        // n_kw entries = n_kw_off - 1 = 134170
        static const char K[] = "character-name-in-title";
        size_t klen = sizeof(K) - 1;
        for (size_t i = 0; i + 1 < n_kw_off; ++i) {
            int64_t lo = kw_off[i];
            int64_t hi = kw_off[i + 1];
            size_t L = (size_t)(hi - lo);
            if (L == klen && std::memcmp(kw_dat.data + lo, K, klen) == 0) {
                target_k_id = (int32_t)(i + 1);
                break;
            }
        }
        if (target_k_id == 0) {
            std::fprintf(stderr, "Could not resolve target keyword\n");
            return 3;
        }
    }

    // ---------- build cn_pass bitset over cn.id ----------
    // cn.id is dense identity 1..234997. Build boolean indexed by id.
    const size_t CN_ROWS = cn_country_code.count;  // 234997
    std::vector<uint8_t> cn_pass(CN_ROWS + 2, 0);  // index by id (1..CN_ROWS)
    {
        GENDB_PHASE("build_cn_pass");
        const int16_t* cc = cn_country_code.data;
        for (size_t i = 0; i < CN_ROWS; ++i) {
            if (cc[i] == target_code) cn_pass[i + 1] = 1;
        }
    }

    // ---------- probe mk via CSR by keyword_id ----------
    // Collect candidate movie_ids, dedup using movie_seen.
    const size_t TITLE_ROWS = title_off.count - 1;  // 2528312
    std::vector<uint8_t> movie_seen(TITLE_ROWS + 2, 0);
    std::vector<int32_t> candidates;

    {
        GENDB_PHASE("csr_probe_mk");
        int32_t lo = mk_kw_idx_off[target_k_id];
        int32_t hi = mk_kw_idx_off[target_k_id + 1];
        candidates.reserve((size_t)(hi - lo));
        for (int32_t p = lo; p < hi; ++p) {
            int32_t mk_row = mk_kw_idx_rowids[p];
            int32_t mv = mk_movie_id[mk_row];
            if (mv > 0 && (size_t)mv <= TITLE_ROWS) {
                if (!movie_seen[mv]) {
                    movie_seen[mv] = 1;
                    candidates.push_back(mv);
                }
            }
        }
    }

    // ---------- for each candidate movie_id, probe mc via offsets, test cn_pass ----------
    // Survivors: movie_ids where some mc row has cn_pass[company_id] == 1.
    std::vector<int32_t> survivors;
    survivors.reserve(candidates.size());

    {
        GENDB_PHASE("probe_mc_and_filter");
        for (int32_t mv : candidates) {
            int32_t lo = mc_mv_idx_off[mv];
            int32_t hi = mc_mv_idx_off[mv + 1];
            bool ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                int32_t cid = mc_company_id[r];
                if (cid > 0 && (size_t)cid <= CN_ROWS && cn_pass[cid]) {
                    ok = true;
                    break;
                }
            }
            if (ok) survivors.push_back(mv);
        }
    }

    // ---------- fetch t.title and compute MIN ----------
    std::string min_title;
    bool has_min = false;
    {
        GENDB_PHASE("fetch_title_min");
        for (int32_t mv : survivors) {
            // t.id is dense identity, row = mv - 1
            int64_t lo = title_off[mv - 1];
            int64_t hi = title_off[mv];
            std::string_view s(title_dat.data + lo, (size_t)(hi - lo));
            if (!has_min || s < std::string_view(min_title)) {
                min_title.assign(s);
                has_min = true;
            }
        }
    }

    // ---------- output CSV ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q2a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 4;
        }
        std::fprintf(fp, "movie_title\n");
        if (has_min) {
            std::fwrite(min_title.data(), 1, min_title.size(), fp);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
