// Q17e: SELECT MIN(n.name) FROM cast_info, company_name, keyword, movie_companies, movie_keyword, name, title
// Plan: resolve keyword + country_code literals, build US-company bitset, drive from movie_keyword aux CSR,
// fuse US-company short-circuit + cast_info expansion + name MIN lookup.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <filesystem>

#include "timing_utils.h"
#include "cli_params.h"

struct Mmap {
    void* ptr = nullptr;
    size_t size = 0;
    Mmap() = default;
    Mmap(const Mmap&) = delete;
    Mmap& operator=(const Mmap&) = delete;
    Mmap(Mmap&& o) noexcept : ptr(o.ptr), size(o.size) { o.ptr = nullptr; o.size = 0; }
    Mmap& operator=(Mmap&& o) noexcept {
        if (this != &o) {
            if (ptr) munmap(ptr, size);
            ptr = o.ptr; size = o.size; o.ptr = nullptr; o.size = 0;
        }
        return *this;
    }
    ~Mmap() { if (ptr) munmap(ptr, size); }
};

static Mmap map_file(const std::string& path) {
    Mmap m;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    m.size = (size_t)st.st_size;
    if (m.size == 0) { close(fd); return m; }
    m.ptr = mmap(nullptr, m.size, PROT_READ, MAP_SHARED, fd, 0);
    if (m.ptr == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    close(fd);
    return m;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--param value ...]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    std::string country_code_eq = gendb::parse_string_arg(argc, argv, "--country_code_eq", "[us]");
    std::string keyword_eq = gendb::parse_string_arg(argc, argv, "--keyword_eq", "character-name-in-title");

    std::filesystem::create_directories(results_dir);

    // ----- mmap all needed files -----
    Mmap m_kw_off   = map_file(gendb_dir + "/keyword/keyword.off");
    Mmap m_kw_dat   = map_file(gendb_dir + "/keyword/keyword.dat");
    Mmap m_kw_id    = map_file(gendb_dir + "/keyword/id.bin");

    Mmap m_cc_dict_off = map_file(gendb_dir + "/company_name/country_code.dict.off");
    Mmap m_cc_dict_dat = map_file(gendb_dir + "/company_name/country_code.dict.dat");
    Mmap m_cc_bin      = map_file(gendb_dir + "/company_name/country_code.bin");
    Mmap m_cn_id       = map_file(gendb_dir + "/company_name/id.bin");

    Mmap m_mk_kw_off  = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
    Mmap m_mk_kw_rids = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
    Mmap m_mk_mid     = map_file(gendb_dir + "/movie_keyword/movie_id.bin");

    Mmap m_mc_mid_off = map_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
    Mmap m_mc_cid     = map_file(gendb_dir + "/movie_companies/company_id.bin");

    Mmap m_ci_mid_off = map_file(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
    Mmap m_ci_pid     = map_file(gendb_dir + "/cast_info/person_id.bin");

    Mmap m_name_id    = map_file(gendb_dir + "/name/id.bin");
    Mmap m_name_off   = map_file(gendb_dir + "/name/name.off");
    Mmap m_name_dat   = map_file(gendb_dir + "/name/name.dat");

    const uint64_t* kw_off = (const uint64_t*)m_kw_off.ptr;
    const char*     kw_dat = (const char*)m_kw_dat.ptr;
    const int32_t*  kw_id  = (const int32_t*)m_kw_id.ptr;
    const uint64_t N_kw    = (m_kw_off.size / 8) - 1;

    const uint64_t* cc_dict_off = (const uint64_t*)m_cc_dict_off.ptr;
    const char*     cc_dict_dat = (const char*)m_cc_dict_dat.ptr;
    const uint64_t  N_cc_dict   = (m_cc_dict_off.size / 8) - 1;
    const int16_t*  cc_bin      = (const int16_t*)m_cc_bin.ptr;
    const int32_t*  cn_id       = (const int32_t*)m_cn_id.ptr;
    const uint64_t  N_cn        = m_cn_id.size / 4;

    const int32_t*  mk_kw_off  = (const int32_t*)m_mk_kw_off.ptr;
    const int32_t*  mk_kw_rids = (const int32_t*)m_mk_kw_rids.ptr;
    const int32_t*  mk_mid     = (const int32_t*)m_mk_mid.ptr;
    const uint64_t  N_mk_kw_off_entries = m_mk_kw_off.size / 4; // max_kw_id + 2

    const int32_t*  mc_mid_off = (const int32_t*)m_mc_mid_off.ptr;
    const int32_t*  mc_cid     = (const int32_t*)m_mc_cid.ptr;
    const uint64_t  N_mc_mid_off_entries = m_mc_mid_off.size / 4; // max_movie_id + 2

    const int32_t*  ci_mid_off = (const int32_t*)m_ci_mid_off.ptr;
    const int32_t*  ci_pid     = (const int32_t*)m_ci_pid.ptr;

    const int32_t*  name_id  = (const int32_t*)m_name_id.ptr;
    const uint64_t* name_off = (const uint64_t*)m_name_off.ptr;
    const char*     name_dat = (const char*)m_name_dat.ptr;
    const uint64_t  N_name   = m_name_id.size / 4;

    { GENDB_PHASE("data_loading"); /* mmap done above */ }
    std::fprintf(stderr, "N_kw=%lu N_cn=%lu N_name=%lu mk_kw_off_entries=%lu mc_mid_off_entries=%lu\n",
                 N_kw, N_cn, N_name, N_mk_kw_off_entries, N_mc_mid_off_entries);
    std::fprintf(stderr, "ci_mid_off_size=%lu name_id_size=%lu\n", m_ci_mid_off.size/4, m_name_id.size);

    std::fprintf(stderr, "[stage] resolve_keyword\n");
    // ----- Resolve keyword literal -----
    int32_t target_kw_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        const size_t klen = keyword_eq.size();
        for (uint64_t r = 0; r < N_kw; ++r) {
            uint64_t lo = kw_off[r], hi = kw_off[r + 1];
            if ((hi - lo) == klen && std::memcmp(kw_dat + lo, keyword_eq.data(), klen) == 0) {
                target_kw_id = kw_id[r];
                break;
            }
        }
    }
    if (target_kw_id < 0) {
        std::fprintf(stderr, "keyword '%s' not found\n", keyword_eq.c_str());
        // Output empty
        std::ofstream out(results_dir + "/Q17e.csv");
        out << "member_in_charnamed_movie\n\n";
        return 0;
    }

    std::fprintf(stderr, "[stage] kw_id=%d, resolve_country_code\n", target_kw_id);
    // ----- Resolve country_code literal to dict id -----
    // Note: country_code.bin encoding uses 1-indexed dict ids (0 = NULL),
    // so the bin value is (position_in_dict_dat + 1).
    int32_t us_dict_id = -1;
    {
        GENDB_PHASE("resolve_country_code");
        const size_t clen = country_code_eq.size();
        for (uint64_t r = 0; r < N_cc_dict; ++r) {
            uint64_t lo = cc_dict_off[r], hi = cc_dict_off[r + 1];
            if ((hi - lo) == clen && std::memcmp(cc_dict_dat + lo, country_code_eq.data(), clen) == 0) {
                us_dict_id = (int32_t)r + 1;
                break;
            }
        }
    }
    if (us_dict_id < 0) {
        std::fprintf(stderr, "country_code '%s' not found\n", country_code_eq.c_str());
        std::ofstream out(results_dir + "/Q17e.csv");
        out << "member_in_charnamed_movie\n\n";
        return 0;
    }

    std::fprintf(stderr, "[stage] us_dict_id=%d, build bitset\n", us_dict_id);
    // ----- Build US-company bitset over company_name.id -----
    // Compute max company id from id.bin
    int32_t max_company_id = 0;
    for (uint64_t r = 0; r < N_cn; ++r) if (cn_id[r] > max_company_id) max_company_id = cn_id[r];
    std::vector<uint64_t> us_company_bitset((size_t)max_company_id / 64 + 2, 0);
    size_t us_company_count = 0;
    {
        GENDB_PHASE("build_us_company_set");
        const int16_t us16 = (int16_t)us_dict_id;
        for (uint64_t r = 0; r < N_cn; ++r) {
            if (cc_bin[r] == us16) {
                int32_t cid = cn_id[r];
                if (cid >= 0) {
                    us_company_bitset[(size_t)cid >> 6] |= (uint64_t)1 << (cid & 63);
                    ++us_company_count;
                }
            }
        }
    }

    std::fprintf(stderr, "[stage] max_company=%d us_companies=%zu build_name_pos\n", max_company_id, us_company_count);
    // ----- Build name pk_pos (id -> row) -----
    // max id
    int32_t max_name_id = 0;
    {
        GENDB_PHASE("build_name_pos");
        for (uint64_t r = 0; r < N_name; ++r) if (name_id[r] > max_name_id) max_name_id = name_id[r];
    }
    std::vector<int32_t> name_pos((size_t)max_name_id + 2, -1);
    {
        GENDB_PHASE("fill_name_pos");
        for (uint64_t r = 0; r < N_name; ++r) {
            int32_t id = name_id[r];
            if (id >= 0) name_pos[(size_t)id] = (int32_t)r;
        }
    }

    std::fprintf(stderr, "[stage] max_name=%d, enumerate_candidates\n", max_name_id);
    // ----- Enumerate candidate movie_ids from movie_keyword aux CSR -----
    std::vector<int32_t> candidate_movies;
    {
        GENDB_PHASE("enumerate_candidate_movies");
        if ((uint64_t)(target_kw_id + 1) < N_mk_kw_off_entries) {
            int32_t lo = mk_kw_off[target_kw_id];
            int32_t hi = mk_kw_off[target_kw_id + 1];
            candidate_movies.reserve((size_t)(hi - lo));
            for (int32_t k = lo; k < hi; ++k) {
                int32_t rid = mk_kw_rids[k];
                int32_t mid = mk_mid[rid];
                if (mid > 0) candidate_movies.push_back(mid);
            }
        }
    }

    // Sort + dedupe to improve cache locality on subsequent CSR probes
    {
        GENDB_PHASE("sort_dedupe_candidates");
        std::sort(candidate_movies.begin(), candidate_movies.end());
        candidate_movies.erase(std::unique(candidate_movies.begin(), candidate_movies.end()), candidate_movies.end());
    }

    // ----- Main scan: per candidate movie, check US-company, expand cast_info, lookup name, update MIN -----
    std::string_view running_min;
    bool have_min = false;
    {
        GENDB_PHASE("main_scan");
        const uint64_t max_mid_idx = N_mc_mid_off_entries - 1; // valid v range: [0, max_mid]
        for (int32_t mid : candidate_movies) {
            if ((uint64_t)mid >= max_mid_idx) continue;

            // US-company short-circuit
            int32_t mc_lo = mc_mid_off[mid];
            int32_t mc_hi = mc_mid_off[mid + 1];
            bool us_hit = false;
            for (int32_t r = mc_lo; r < mc_hi; ++r) {
                int32_t cid = mc_cid[r];
                if (cid >= 0 && cid <= max_company_id &&
                    (us_company_bitset[(size_t)cid >> 6] & ((uint64_t)1 << (cid & 63)))) {
                    us_hit = true;
                    break;
                }
            }
            if (!us_hit) continue;

            // cast_info expansion
            int32_t ci_lo = ci_mid_off[mid];
            int32_t ci_hi = ci_mid_off[mid + 1];
            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                int32_t pid = ci_pid[r];
                if (pid < 0 || pid > max_name_id) continue;
                int32_t nrow = name_pos[(size_t)pid];
                if (nrow < 0) continue;
                uint64_t nlo = name_off[nrow], nhi = name_off[nrow + 1];
                std::string_view nm(name_dat + nlo, nhi - nlo);
                if (nm.empty()) continue;
                if (!have_min) {
                    running_min = nm;
                    have_min = true;
                } else if (nm < running_min) {
                    running_min = nm;
                }
            }
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::ofstream out(results_dir + "/Q17e.csv");
        out << "member_in_charnamed_movie\n";
        if (have_min) {
            // CSV-quote if contains comma, quote, or newline
            bool need_quote = false;
            for (char c : running_min) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (need_quote) {
                out << '"';
                for (char c : running_min) {
                    if (c == '"') out << "\"\"";
                    else out << c;
                }
                out << '"';
            } else {
                out.write(running_min.data(), running_min.size());
            }
            out << "\n";
        } else {
            out << "\n";
        }
    }

    std::fprintf(stderr, "kw_id=%d us_dict_id=%d us_companies=%zu candidates=%zu\n",
                 target_kw_id, us_dict_id, us_company_count, candidate_movies.size());

    return 0;
}
