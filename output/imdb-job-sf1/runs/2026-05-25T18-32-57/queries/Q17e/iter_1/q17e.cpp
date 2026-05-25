// Q17e: SELECT MIN(n.name) FROM ci,cn,k,mc,mk,n,t WHERE cn.country_code='[us]' AND k.keyword='character-name-in-title' ...
#include "timing_utils.h"
#include "cli_params.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

static const void* mmap_file(const std::string& path, size_t* out_size = nullptr) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(fd, &st) != 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    if (out_size) *out_size = (size_t)st.st_size;
    void* p = ::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    return p;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [--country_code_eq STR] [--keyword_eq STR]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    std::string country_code_eq = gendb::parse_string_arg(argc, argv, "--country_code_eq", "[us]");
    std::string keyword_eq      = gendb::parse_string_arg(argc, argv, "--keyword_eq",      "character-name-in-title");

    // mmap pointers
    const uint64_t* kw_off = nullptr;
    const char*     kw_dat = nullptr;
    const int32_t*  kw_ids = nullptr;
    size_t          kw_n   = 0;

    const uint64_t* cc_dict_off = nullptr;
    const char*     cc_dict_dat = nullptr;
    size_t          cc_dict_n   = 0;
    const int16_t*  cn_country  = nullptr;
    const int32_t*  cn_id       = nullptr;
    size_t          cn_n        = 0;

    const int32_t* mk_kw_off    = nullptr;
    const int32_t* mk_kw_rowids = nullptr;
    const int32_t* mk_movie_id  = nullptr;
    size_t         mk_kw_off_n  = 0;

    const int32_t* mc_movie_off = nullptr;  // primary CSR on movie_id
    size_t         mc_movie_off_n = 0;
    const int32_t* mc_company_id = nullptr;

    const int32_t* ci_movie_off = nullptr;  // primary CSR on movie_id
    size_t         ci_movie_off_n = 0;
    const int32_t* ci_person_id = nullptr;

    const int32_t* name_id    = nullptr;
    const uint64_t* name_off  = nullptr;
    const char*    name_dat   = nullptr;
    const int32_t* name_order = nullptr;
    size_t         name_n     = 0;

    {
        GENDB_PHASE("data_loading");
        size_t sz;

        kw_off = (const uint64_t*)mmap_file(gendb_dir + "/keyword/keyword.off", &sz);
        kw_n   = sz / 8 - 1;
        kw_dat = (const char*)mmap_file(gendb_dir + "/keyword/keyword.dat");
        kw_ids = (const int32_t*)mmap_file(gendb_dir + "/keyword/id.bin");

        cc_dict_off = (const uint64_t*)mmap_file(gendb_dir + "/company_name/country_code.dict.off", &sz);
        cc_dict_n   = sz / 8 - 1;
        cc_dict_dat = (const char*)mmap_file(gendb_dir + "/company_name/country_code.dict.dat");
        cn_country  = (const int16_t*)mmap_file(gendb_dir + "/company_name/country_code.bin", &sz);
        cn_n        = sz / 2;
        cn_id       = (const int32_t*)mmap_file(gendb_dir + "/company_name/id.bin");

        mk_kw_off    = (const int32_t*)mmap_file(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin", &sz);
        mk_kw_off_n  = sz / 4;
        mk_kw_rowids = (const int32_t*)mmap_file(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id  = (const int32_t*)mmap_file(gendb_dir + "/movie_keyword/movie_id.bin");

        mc_movie_off   = (const int32_t*)mmap_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin", &sz);
        mc_movie_off_n = sz / 4;
        mc_company_id  = (const int32_t*)mmap_file(gendb_dir + "/movie_companies/company_id.bin");

        ci_movie_off   = (const int32_t*)mmap_file(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin", &sz);
        ci_movie_off_n = sz / 4;
        ci_person_id   = (const int32_t*)mmap_file(gendb_dir + "/cast_info/person_id.bin");

        name_id    = (const int32_t*)mmap_file(gendb_dir + "/name/id.bin", &sz);
        name_n     = sz / 4;
        name_off   = (const uint64_t*)mmap_file(gendb_dir + "/name/name.off");
        name_dat   = (const char*)mmap_file(gendb_dir + "/name/name.dat");
        name_order = (const int32_t*)mmap_file(gendb_dir + "/column_versions/name.name.sortorder/order.bin");
    }

    std::string min_name;
    bool found = false;

    {
        GENDB_PHASE("main_scan");

        // ----- Step 1: resolve keyword literal -> target_kw_id -----
        int32_t target_kw_id = -1;
        {
            const size_t klen = keyword_eq.size();
            const char* kbuf = keyword_eq.data();
            for (size_t r = 0; r < kw_n; ++r) {
                uint64_t lo = kw_off[r], hi = kw_off[r+1];
                if ((hi - lo) == klen && std::memcmp(kw_dat + lo, kbuf, klen) == 0) {
                    target_kw_id = kw_ids[r];
                    break;
                }
            }
        }
        if (target_kw_id < 0) {
            std::fprintf(stderr, "keyword '%s' not found\n", keyword_eq.c_str());
        }

        // ----- Step 2a: resolve country_code dict id -----
        int16_t us_dict_id = -1;
        {
            const size_t clen = country_code_eq.size();
            const char* cbuf = country_code_eq.data();
            for (size_t i = 0; i < cc_dict_n; ++i) {
                uint64_t lo = cc_dict_off[i], hi = cc_dict_off[i+1];
                if ((hi - lo) == clen && std::memcmp(cc_dict_dat + lo, cbuf, clen) == 0) {
                    // Storage uses 1-indexed dict codes: bin value k => dict[k-1]
                    us_dict_id = (int16_t)(i + 1);
                    break;
                }
            }
        }
        if (us_dict_id < 0) {
            std::fprintf(stderr, "country_code '%s' not found in dict\n", country_code_eq.c_str());
        }

        // ----- Step 2b: build us_company_bitset -----
        int32_t max_company_id = 0;
        for (size_t i = 0; i < cn_n; ++i) {
            int32_t id = cn_id[i];
            if (id > max_company_id) max_company_id = id;
        }
        size_t us_bitset_bytes = ((size_t)max_company_id / 8) + 1;
        std::vector<uint8_t> us_bitset(us_bitset_bytes, 0);
        if (us_dict_id >= 0) {
            for (size_t r = 0; r < cn_n; ++r) {
                if (cn_country[r] == us_dict_id) {
                    int32_t cid = cn_id[r];
                    if (cid >= 0) {
                        us_bitset[(size_t)cid >> 3] |= (uint8_t)(1u << (cid & 7));
                    }
                }
            }
        }

        // ----- Step 3: enumerate candidate movies via aux CSR -----
        std::vector<int32_t> candidate_movies;
        if (target_kw_id >= 0 && (size_t)(target_kw_id + 1) < mk_kw_off_n) {
            int32_t lo = mk_kw_off[target_kw_id];
            int32_t hi = mk_kw_off[target_kw_id + 1];
            candidate_movies.reserve((size_t)(hi - lo));
            for (int32_t k = lo; k < hi; ++k) {
                int32_t rowid = mk_kw_rowids[k];
                candidate_movies.push_back(mk_movie_id[rowid]);
            }
            // Rowids are in row order; movie_keyword is physically sorted by movie_id,
            // so candidate_movies is non-decreasing. Dedupe.
            candidate_movies.erase(std::unique(candidate_movies.begin(), candidate_movies.end()),
                                   candidate_movies.end());
        }

        // ----- Step 4: filter by US company, expand to person_id bitset -----
        int32_t max_name_id = 0;
        for (size_t i = 0; i < name_n; ++i) {
            int32_t id = name_id[i];
            if (id > max_name_id) max_name_id = id;
        }
        size_t pid_bitset_bytes = ((size_t)max_name_id / 8) + 1;
        std::vector<uint8_t> pid_bitset(pid_bitset_bytes, 0);

        const size_t mc_off_max = mc_movie_off_n - 1;
        const size_t ci_off_max = ci_movie_off_n - 1;

        for (int32_t mid : candidate_movies) {
            if (mid < 0 || (size_t)(mid + 1) > mc_off_max) continue;
            int32_t mc_lo = mc_movie_off[mid];
            int32_t mc_hi = mc_movie_off[mid + 1];
            bool has_us = false;
            for (int32_t r = mc_lo; r < mc_hi; ++r) {
                int32_t cid = mc_company_id[r];
                if (cid >= 0 && cid <= max_company_id) {
                    if (us_bitset[(size_t)cid >> 3] & (uint8_t)(1u << (cid & 7))) {
                        has_us = true;
                        break;
                    }
                }
            }
            if (!has_us) continue;

            if ((size_t)(mid + 1) > ci_off_max) continue;
            int32_t ci_lo = ci_movie_off[mid];
            int32_t ci_hi = ci_movie_off[mid + 1];
            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                int32_t pid = ci_person_id[r];
                if (pid >= 0 && pid <= max_name_id) {
                    pid_bitset[(size_t)pid >> 3] |= (uint8_t)(1u << (pid & 7));
                }
            }
        }

        // ----- Step 5: find MIN(n.name) by sorted-order first hit -----
        for (size_t k = 0; k < name_n; ++k) {
            int32_t nrow = name_order[k];
            if (nrow < 0) continue;
            int32_t pid = name_id[nrow];
            if (pid < 0 || pid > max_name_id) continue;
            if (pid_bitset[(size_t)pid >> 3] & (uint8_t)(1u << (pid & 7))) {
                uint64_t lo = name_off[nrow], hi = name_off[nrow + 1];
                min_name.assign(name_dat + lo, (size_t)(hi - lo));
                found = true;
                break;
            }
        }
    }

    {
        GENDB_PHASE("output");
        {
            std::string cmd = "mkdir -p '" + results_dir + "'";
            int rc = std::system(cmd.c_str());
            (void)rc;
        }
        std::string out_path = results_dir + "/Q17e.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "fopen failed: %s\n", out_path.c_str()); return 1; }
        std::fprintf(fp, "member_in_charnamed_movie\n");
        if (found) {
            bool need_quote = false;
            for (char c : min_name) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (need_quote) {
                std::fputc('"', fp);
                for (char c : min_name) {
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
                std::fputc('\n', fp);
            } else {
                std::fprintf(fp, "%s\n", min_name.c_str());
            }
        } else {
            std::fprintf(fp, "\n");
        }
        std::fclose(fp);
    }

    return 0;
}
