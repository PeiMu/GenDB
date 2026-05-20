// Q17a — character-name-in-title keyword, US company, person name LIKE 'B%'
// MIN(n.name) over matching persons. Drive from keyword CSR (10 mk rows).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "timing_utils.h"

namespace fs = std::filesystem;

struct Mapped {
    const void* ptr = nullptr;
    size_t len = 0;
};

static Mapped mmap_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", path.c_str());
        std::exit(2);
    }
    struct stat st{};
    fstat(fd, &st);
    void* p = nullptr;
    if (st.st_size > 0) {
        p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "mmap failed: %s\n", path.c_str());
            std::exit(2);
        }
    }
    close(fd);
    return {p, (size_t)st.st_size};
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    fs::create_directories(results);

    // --- Data loading ---
    Mapped m_kw_off, m_kw_dat;
    Mapped m_cc_doff, m_cc_ddat, m_cn_cc;
    Mapped m_mk_koff, m_mk_krow, m_mk_mv;
    Mapped m_mc_off, m_mc_cid;
    Mapped m_ci_off, m_ci_pid;
    Mapped m_n_off, m_n_dat;

    {
        GENDB_PHASE("data_loading");
        m_kw_off  = mmap_file(store + "/keyword/keyword.off");
        m_kw_dat  = mmap_file(store + "/keyword/keyword.dat");

        m_cc_doff = mmap_file(store + "/company_name/country_code.dict.off");
        m_cc_ddat = mmap_file(store + "/company_name/country_code.dict.dat");
        m_cn_cc   = mmap_file(store + "/company_name/country_code.bin");

        m_mk_koff = mmap_file(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        m_mk_krow = mmap_file(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        m_mk_mv   = mmap_file(store + "/movie_keyword/movie_id.bin");

        m_mc_off  = mmap_file(store + "/_idx/movie_companies__movie_id__offsets.bin");
        m_mc_cid  = mmap_file(store + "/movie_companies/company_id.bin");

        m_ci_off  = mmap_file(store + "/_idx/cast_info__movie_id__offsets.bin");
        m_ci_pid  = mmap_file(store + "/cast_info/person_id.bin");

        m_n_off   = mmap_file(store + "/name/name.off");
        m_n_dat   = mmap_file(store + "/name/name.dat");
    }

    // Pointers / counts
    const int64_t* kw_off  = (const int64_t*)m_kw_off.ptr;
    int64_t kw_n           = (int64_t)(m_kw_off.len / sizeof(int64_t)) - 1; // 134170
    const char*    kw_dat  = (const char*)m_kw_dat.ptr;

    const int64_t* cc_doff = (const int64_t*)m_cc_doff.ptr;
    int64_t cc_n           = (int64_t)(m_cc_doff.len / sizeof(int64_t)) - 1;
    const char*    cc_ddat = (const char*)m_cc_ddat.ptr;
    const int16_t* cn_cc   = (const int16_t*)m_cn_cc.ptr;
    // int64_t cn_n           = (int64_t)(m_cn_cc.len / sizeof(int16_t));

    const int32_t* mk_koff = (const int32_t*)m_mk_koff.ptr;
    const int32_t* mk_krow = (const int32_t*)m_mk_krow.ptr;
    const int32_t* mk_mv   = (const int32_t*)m_mk_mv.ptr;

    const int32_t* mc_off  = (const int32_t*)m_mc_off.ptr;
    const int32_t* mc_cid  = (const int32_t*)m_mc_cid.ptr;
    int32_t mc_movies      = (int32_t)(m_mc_off.len / sizeof(int32_t)) - 2; // first entry is sentinel? convention?
    (void)mc_movies;

    const int32_t* ci_off  = (const int32_t*)m_ci_off.ptr;
    const int32_t* ci_pid  = (const int32_t*)m_ci_pid.ptr;

    const int64_t* n_off   = (const int64_t*)m_n_off.ptr;
    int64_t n_rows         = (int64_t)(m_n_off.len / sizeof(int64_t)) - 1; // 4167491
    const char*    n_dat   = (const char*)m_n_dat.ptr;

    // --- Resolve constants ---
    // k_id is the 1-based keyword_id (matches CSR offsets convention: offsets[k_id])
    int32_t k_id = -1;
    {
        GENDB_PHASE("resolve_kid");
        const char* target = "character-name-in-title";
        size_t tlen = std::strlen(target);
        for (int64_t i = 0; i < kw_n; ++i) {
            int64_t a = kw_off[i], b = kw_off[i + 1];
            if ((size_t)(b - a) == tlen && std::memcmp(kw_dat + a, target, tlen) == 0) {
                k_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    // us_code: code 0 = NULL, real codes 1..K (where dict index i corresponds to code i+1)
    int16_t us_code = -1;
    {
        GENDB_PHASE("resolve_us_code");
        const char* target = "[us]";
        size_t tlen = std::strlen(target);
        for (int64_t i = 0; i < cc_n; ++i) {
            int64_t a = cc_doff[i], b = cc_doff[i + 1];
            if ((size_t)(b - a) == tlen && std::memcmp(cc_ddat + a, target, tlen) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }
    }

    // Output writer
    std::string out_path = results + "/Q17a.csv";

    if (k_id < 0 || us_code < 0) {
        FILE* f = std::fopen(out_path.c_str(), "w");
        std::fprintf(f, "member_in_charnamed_american_movie,a1\n");
        std::fclose(f);
        return 0;
    }

    // --- Build b_prefix bitset in parallel ---
    std::vector<uint8_t> b_prefix(n_rows, 0);
    {
        GENDB_PHASE("build_b_prefix");
        int nthreads = (int)std::thread::hardware_concurrency();
        if (nthreads <= 0) nthreads = 4;
        if (nthreads > 12) nthreads = 12;
        std::vector<std::thread> ths;
        ths.reserve(nthreads);
        int64_t chunk = (n_rows + nthreads - 1) / nthreads;
        for (int t = 0; t < nthreads; ++t) {
            int64_t lo = (int64_t)t * chunk;
            int64_t hi = std::min(n_rows, lo + chunk);
            if (lo >= hi) break;
            ths.emplace_back([=, &b_prefix]() {
                for (int64_t i = lo; i < hi; ++i) {
                    int64_t a = n_off[i], b = n_off[i + 1];
                    b_prefix[i] = (b - a >= 1 && n_dat[a] == 'B') ? 1 : 0;
                }
            });
        }
        for (auto& th : ths) th.join();
    }

    // --- Scan mk CSR for keyword ---
    std::vector<int32_t> cand_movies;
    {
        GENDB_PHASE("mk_csr_scan");
        int32_t lo = mk_koff[k_id];
        int32_t hi = mk_koff[k_id + 1];
        cand_movies.reserve(hi - lo);
        for (int32_t i = lo; i < hi; ++i) {
            int32_t r = mk_krow[i];
            cand_movies.push_back(mk_mv[r]);
        }
        std::sort(cand_movies.begin(), cand_movies.end());
        cand_movies.erase(std::unique(cand_movies.begin(), cand_movies.end()), cand_movies.end());
    }

    // --- For each candidate, probe mc then ci ---
    std::string_view min_name_sv;
    bool have_min = false;

    {
        GENDB_PHASE("main_scan");
        for (int32_t mv : cand_movies) {
            // Probe mc: any company with country_code == us_code?
            // offsets convention: offsets[mv] .. offsets[mv+1] for 1-based movie_id mv
            int32_t mlo = mc_off[mv];
            int32_t mhi = mc_off[mv + 1];
            bool us_match = false;
            for (int32_t r = mlo; r < mhi; ++r) {
                int32_t cid = mc_cid[r];
                if (cn_cc[cid - 1] == us_code) { us_match = true; break; }
            }
            if (!us_match) continue;

            // Probe ci: for each person with B-prefix, update MIN
            int32_t clo = ci_off[mv];
            int32_t chi = ci_off[mv + 1];
            for (int32_t r = clo; r < chi; ++r) {
                int32_t pid = ci_pid[r];
                if (!b_prefix[pid - 1]) continue;
                int64_t a = n_off[pid - 1], b = n_off[pid];
                std::string_view sv(n_dat + a, (size_t)(b - a));
                if (!have_min || sv < min_name_sv) {
                    min_name_sv = sv;
                    have_min = true;
                }
            }
        }
    }

    // --- Output CSV ---
    {
        GENDB_PHASE("output");
        FILE* f = std::fopen(out_path.c_str(), "w");
        std::fprintf(f, "member_in_charnamed_american_movie,a1\n");
        if (have_min) {
            // Quote both fields if they contain a comma or quote; else emit raw.
            std::string s(min_name_sv);
            bool need_quote = s.find(',') != std::string::npos ||
                              s.find('"') != std::string::npos ||
                              s.find('\n') != std::string::npos;
            std::string esc;
            if (need_quote) {
                esc.reserve(s.size() + 2);
                esc.push_back('"');
                for (char c : s) {
                    if (c == '"') esc.push_back('"');
                    esc.push_back(c);
                }
                esc.push_back('"');
                std::fprintf(f, "%s,%s\n", esc.c_str(), esc.c_str());
            } else {
                std::fprintf(f, "%s,%s\n", s.c_str(), s.c_str());
            }
        }
        std::fclose(f);
    }

    return 0;
}
