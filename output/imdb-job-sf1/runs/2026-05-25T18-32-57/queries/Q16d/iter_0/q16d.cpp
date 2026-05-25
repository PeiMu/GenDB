// Q16d implementation
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_set>
#include <fstream>
#include <filesystem>

#include "timing_utils.h"
#include "cli_params.h"

namespace fs = std::filesystem;

static void* map_file(const std::string& path, size_t& out_sz) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(2); }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(2); }
    out_sz = (size_t)st.st_size;
    void* p = ::mmap(nullptr, out_sz, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(2); }
    ::close(fd);
    return p;
}

static std::string csv_quote(std::string_view s) {
    bool need = false;
    for (char c : s) { if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; } }
    if (!need) return std::string(s);
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [--params...]\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string out_dir = argv[2];

    // Params
    int64_t episode_lower = gendb::parse_int_arg(argc, argv, "--episode_nr_lower", 5);
    int64_t episode_upper = gendb::parse_int_arg(argc, argv, "--episode_nr_upper", 100);
    std::string country_code_eq = gendb::parse_string_arg(argc, argv, "--country_code_eq", "[us]");
    std::string keyword_eq      = gendb::parse_string_arg(argc, argv, "--keyword_eq", "character-name-in-title");

    // mmap files
    size_t sz;

    // Keyword dim
    size_t kw_id_sz, kw_off_sz, kw_dat_sz;
    const int32_t* kw_id   = (const int32_t*)map_file(gendb + "/keyword/id.bin",      kw_id_sz);
    const uint64_t* kw_off = (const uint64_t*)map_file(gendb + "/keyword/keyword.off", kw_off_sz);
    const char*    kw_dat  = (const char*)    map_file(gendb + "/keyword/keyword.dat", kw_dat_sz);
    uint64_t N_kw = kw_id_sz / 4;

    // Company name dim: country_code dict + code col + id col
    size_t cn_cc_sz, cn_dict_off_sz, cn_dict_dat_sz, cn_id_sz;
    const int16_t* cn_cc       = (const int16_t*) map_file(gendb + "/company_name/country_code.bin",      cn_cc_sz);
    const uint64_t* cn_dict_off= (const uint64_t*)map_file(gendb + "/company_name/country_code.dict.off", cn_dict_off_sz);
    const char*    cn_dict_dat = (const char*)    map_file(gendb + "/company_name/country_code.dict.dat", cn_dict_dat_sz);
    const int32_t* cn_id       = (const int32_t*) map_file(gendb + "/company_name/id.bin",                cn_id_sz);
    uint64_t N_cn = cn_id_sz / 4;
    uint64_t N_cn_dict = cn_dict_off_sz / 8 - 1;

    // Title dim
    size_t t_id_sz, t_ep_sz, t_title_off_sz, t_title_dat_sz;
    const int32_t* t_id      = (const int32_t*) map_file(gendb + "/title/id.bin", t_id_sz);
    const int32_t* t_episode = (const int32_t*) map_file(gendb + "/title/episode_nr.bin", t_ep_sz);
    const uint64_t* t_t_off  = (const uint64_t*)map_file(gendb + "/title/title.off",      t_title_off_sz);
    const char*    t_t_dat   = (const char*)    map_file(gendb + "/title/title.dat",      t_title_dat_sz);
    uint64_t N_t = t_id_sz / 4;
    (void)t_id;

    // movie_keyword aux CSR on keyword_id
    size_t mk_off_sz, mk_rows_sz, mk_mid_sz;
    const int32_t* mk_kw_off  = (const int32_t*)map_file(gendb + "/_idx/movie_keyword__keyword_id__offsets.bin", mk_off_sz);
    const int32_t* mk_kw_rows = (const int32_t*)map_file(gendb + "/_idx/movie_keyword__keyword_id__rowids.bin", mk_rows_sz);
    const int32_t* mk_movie_id= (const int32_t*)map_file(gendb + "/movie_keyword/movie_id.bin", mk_mid_sz);

    // movie_companies primary CSR on movie_id + company_id
    size_t mc_off_sz, mc_cid_sz;
    const int32_t* mc_off    = (const int32_t*) map_file(gendb + "/_idx/movie_companies__movie_id__offsets.bin", mc_off_sz);
    const int32_t* mc_cid    = (const int32_t*) map_file(gendb + "/movie_companies/company_id.bin", mc_cid_sz);

    // cast_info primary CSR on movie_id + person_id
    size_t ci_off_sz, ci_pid_sz;
    const int32_t* ci_off    = (const int32_t*) map_file(gendb + "/_idx/cast_info__movie_id__offsets.bin", ci_off_sz);
    const int32_t* ci_pid    = (const int32_t*) map_file(gendb + "/cast_info/person_id.bin", ci_pid_sz);

    // aka_name primary CSR on person_id + name
    size_t an_off_sz, an_n_off_sz, an_n_dat_sz;
    const int32_t* an_off    = (const int32_t*) map_file(gendb + "/_idx/aka_name__person_id__offsets.bin", an_off_sz);
    const uint64_t* an_n_off = (const uint64_t*)map_file(gendb + "/aka_name/name.off", an_n_off_sz);
    const char*    an_n_dat  = (const char*)    map_file(gendb + "/aka_name/name.dat", an_n_dat_sz);

    sz = 0; (void)sz;

    // ---- Resolve literals ----
    int32_t target_kw_id = -1;
    {
        GENDB_PHASE("resolve_literals");
        // keyword
        for (uint64_t r = 0; r < N_kw; ++r) {
            uint64_t lo = kw_off[r], hi = kw_off[r+1];
            if ((hi - lo) == keyword_eq.size() && std::memcmp(kw_dat + lo, keyword_eq.data(), keyword_eq.size()) == 0) {
                target_kw_id = kw_id[r];
                break;
            }
        }
        if (target_kw_id < 0) {
            std::fprintf(stderr, "keyword literal not found\n");
            return 3;
        }
    }

    // us_code: scan dict to find code
    int16_t us_code = 0;
    {
        for (uint64_t i = 0; i < N_cn_dict; ++i) {
            uint64_t lo = cn_dict_off[i], hi = cn_dict_off[i+1];
            if ((hi - lo) == country_code_eq.size() && std::memcmp(cn_dict_dat + lo, country_code_eq.data(), country_code_eq.size()) == 0) {
                us_code = (int16_t)(i + 1); // code 0 = NULL, codes start at 1
                break;
            }
        }
        if (us_code == 0) {
            std::fprintf(stderr, "country_code literal not found\n");
            return 3;
        }
    }

    // ---- Build cn_us_bitset over company_name.id ----
    // max company id = max value of cn_id[r], approx 234,997
    uint32_t max_company_id = 0;
    for (uint64_t r = 0; r < N_cn; ++r) {
        if ((uint32_t)cn_id[r] > max_company_id) max_company_id = (uint32_t)cn_id[r];
    }
    std::vector<uint64_t> cn_us_bits((max_company_id + 64) / 64 + 1, 0);
    {
        GENDB_PHASE("build_cn_us_bitset");
        for (uint64_t r = 0; r < N_cn; ++r) {
            if (cn_cc[r] == us_code) {
                uint32_t id = (uint32_t)cn_id[r];
                cn_us_bits[id >> 6] |= (1ULL << (id & 63));
            }
        }
    }
    auto test_cn = [&](int32_t id) -> bool {
        if (id < 0 || (uint32_t)id > max_company_id) return false;
        return (cn_us_bits[(uint32_t)id >> 6] >> ((uint32_t)id & 63)) & 1ULL;
    };

    // ---- Drive movie_keyword aux CSR ----
    int32_t mk_lo = mk_kw_off[target_kw_id];
    int32_t mk_hi = mk_kw_off[target_kw_id + 1];

    // dedupe movie_ids and filter via title.episode_nr and mc semi-join
    // Track which movie_ids we've processed
    // max movie id is up to ~2,528,312
    uint64_t max_movie_id = N_t; // dense 1..N_t
    std::vector<uint8_t> seen((max_movie_id + 2), 0);

    std::vector<int32_t> surviving_movies;
    surviving_movies.reserve(256);

    int32_t epi_lo = (int32_t)episode_lower;
    int32_t epi_hi = (int32_t)episode_upper;

    {
        GENDB_PHASE("filter_and_semi_join");
        for (int32_t k = mk_lo; k < mk_hi; ++k) {
            int32_t mk_row = mk_kw_rows[k];
            int32_t v = mk_movie_id[mk_row];
            if (v <= 0 || (uint64_t)v > max_movie_id) continue;
            if (seen[(uint32_t)v]) continue;
            seen[(uint32_t)v] = 1;

            // title.episode_nr filter; title.id is dense 1..N_t so row = v-1
            int32_t row = v - 1;
            int32_t en = t_episode[row];
            if (en < epi_lo || en >= epi_hi) continue;

            // semi-join mc.company_id in cn_us
            int32_t lo = mc_off[v];
            int32_t hi = mc_off[v + 1];
            bool found = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (test_cn(mc_cid[r])) { found = true; break; }
            }
            if (!found) continue;

            surviving_movies.push_back(v);
        }
    }

    // ---- Expand cast_info, semi-join aka_name, aggregate ----
    std::string min_name;
    bool have_name = false;
    std::string min_title;
    bool have_title = false;

    {
        GENDB_PHASE("expand_and_aggregate");
        for (int32_t v : surviving_movies) {
            // Read title once
            int32_t row = v - 1;
            uint64_t t_lo = t_t_off[row], t_hi = t_t_off[row + 1];
            std::string_view tv(t_t_dat + t_lo, t_hi - t_lo);
            if (!have_title || tv < std::string_view(min_title)) {
                min_title.assign(tv);
                have_title = true;
            }

            // cast_info range
            int32_t cilo = ci_off[v];
            int32_t cihi = ci_off[v + 1];
            for (int32_t r = cilo; r < cihi; ++r) {
                int32_t pid = ci_pid[r];
                if (pid <= 0) continue;

                // aka_name range via primary CSR on person_id
                int32_t alo = an_off[pid];
                int32_t ahi = an_off[pid + 1];
                if (alo == ahi) continue;

                for (int32_t ar = alo; ar < ahi; ++ar) {
                    uint64_t no = an_n_off[ar], nh = an_n_off[ar + 1];
                    if (no == nh) continue; // null
                    std::string_view nv(an_n_dat + no, nh - no);
                    if (!have_name || nv < std::string_view(min_name)) {
                        min_name.assign(nv);
                        have_name = true;
                    }
                }
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        fs::create_directories(out_dir);
        std::ofstream out(out_dir + "/Q16d.csv", std::ios::binary);
        out << "cool_actor_pseudonym,series_named_after_char\n";
        if (have_name) out << csv_quote(min_name);
        out << ",";
        if (have_title) out << csv_quote(min_title);
        out << "\n";
        out.close();
    }

    return 0;
}
