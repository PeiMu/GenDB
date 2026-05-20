// Q2b: MIN(t.title) where cn.country_code='[nl]' AND k.keyword='character-name-in-title'
//
// Pipeline:
//  1. Resolve [nl] -> int16 country code, build cn_pass bitmap over cn ids.
//  2. Resolve target_k_id by scanning keyword.dat.
//  3. Enumerate mk rows via movie_keyword__keyword_id CSR.
//  4. For each candidate movie, probe movie_companies via offsets and check cn_pass.
//  5. Read title via title.off/title.dat and track min lexicographic title.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"

struct Mapped {
    const void* base = nullptr;
    size_t size = 0;
};

static Mapped map_file(const std::string& path) {
    Mapped m;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open %s failed\n", path.c_str());
        std::exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::fprintf(stderr, "fstat %s failed\n", path.c_str());
        std::exit(1);
    }
    m.size = (size_t)st.st_size;
    if (m.size == 0) { m.base = nullptr; ::close(fd); return m; }
    void* p = mmap(nullptr, m.size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "mmap %s failed\n", path.c_str());
        std::exit(1);
    }
    m.base = p;
    ::close(fd);
    return m;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    // ---- Data loading ----
    Mapped m_cn_cc, m_cn_cc_off, m_cn_cc_dat;
    Mapped m_k_off, m_k_dat;
    Mapped m_mk_off, m_mk_rowids, m_mk_movie_id;
    Mapped m_mc_off, m_mc_company_id;
    Mapped m_t_off, m_t_dat;

    {
        GENDB_PHASE("data_loading");
        m_cn_cc       = map_file(gendb_dir + "/company_name/country_code.bin");
        m_cn_cc_off   = map_file(gendb_dir + "/company_name/country_code.dict.off");
        m_cn_cc_dat   = map_file(gendb_dir + "/company_name/country_code.dict.dat");

        m_k_off       = map_file(gendb_dir + "/keyword/keyword.off");
        m_k_dat       = map_file(gendb_dir + "/keyword/keyword.dat");

        m_mk_off      = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        m_mk_rowids   = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        m_mk_movie_id = map_file(gendb_dir + "/movie_keyword/movie_id.bin");

        m_mc_off       = map_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        m_mc_company_id= map_file(gendb_dir + "/movie_companies/company_id.bin");

        m_t_off       = map_file(gendb_dir + "/title/title.off");
        m_t_dat       = map_file(gendb_dir + "/title/title.dat");
    }

    const int16_t* cn_country_code = (const int16_t*)m_cn_cc.base;
    const size_t cn_rows = m_cn_cc.size / sizeof(int16_t); // 234997
    const int64_t* cc_off = (const int64_t*)m_cn_cc_off.base;
    const size_t cc_off_count = m_cn_cc_off.size / sizeof(int64_t);
    const char* cc_dat = (const char*)m_cn_cc_dat.base;

    const int64_t* k_off = (const int64_t*)m_k_off.base;
    const size_t k_off_count = m_k_off.size / sizeof(int64_t);
    const char* k_dat = (const char*)m_k_dat.base;

    const int32_t* mk_csr_off = (const int32_t*)m_mk_off.base;
    const int32_t* mk_csr_rowids = (const int32_t*)m_mk_rowids.base;
    const int32_t* mk_movie_id_bin = (const int32_t*)m_mk_movie_id.base;

    const int32_t* mc_csr_off = (const int32_t*)m_mc_off.base;
    const int32_t* mc_company_id_bin = (const int32_t*)m_mc_company_id.base;
    const size_t mc_off_count = m_mc_off.size / sizeof(int32_t);
    const size_t title_rows_capacity = mc_off_count - 1;
    (void)title_rows_capacity;

    const int64_t* t_off = (const int64_t*)m_t_off.base;
    const char* t_dat = (const char*)m_t_dat.base;

    // ---- Resolve [nl] -> target_code ----
    int16_t target_code = 0;
    {
        // dict layout: row i string at [off[i], off[i+1]), code = i+1 (code 0 = NULL)
        const size_t n = (cc_off_count >= 1) ? (cc_off_count - 1) : 0;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = cc_off[i];
            int64_t hi = cc_off[i+1];
            std::string_view s(cc_dat + lo, (size_t)(hi - lo));
            if (s == "[nl]") {
                target_code = (int16_t)(i + 1);
                break;
            }
        }
        if (target_code == 0) {
            std::fprintf(stderr, "[nl] not found in country_code dict\n");
        }
    }

    // ---- Build cn_pass bitmap ----
    // company_name.id is 1-based dense identity (row i -> id i+1).
    // Index cn_pass by company_id directly, so allocate cn_rows+1 and write to slot id.
    std::vector<uint8_t> cn_pass(cn_rows + 1, 0);
    {
        GENDB_PHASE("build_cn_pass");
        for (size_t i = 0; i < cn_rows; ++i) {
            cn_pass[i + 1] = (cn_country_code[i] == target_code) ? 1 : 0;
        }
    }

    // ---- Resolve target_k_id by scanning keyword.dat ----
    int32_t target_k_id = -1;
    {
        GENDB_PHASE("resolve_keyword");
        const std::string_view target_kw("character-name-in-title");
        const size_t k_rows = (k_off_count >= 1) ? (k_off_count - 1) : 0;
        for (size_t i = 0; i < k_rows; ++i) {
            int64_t lo = k_off[i];
            int64_t hi = k_off[i+1];
            std::string_view s(k_dat + lo, (size_t)(hi - lo));
            if (s == target_kw) {
                target_k_id = (int32_t)(i + 1); // keyword.id is 1-based dense identity
                break;
            }
        }
        if (target_k_id < 0) {
            std::fprintf(stderr, "keyword '%.*s' not found\n",
                         (int)target_kw.size(), target_kw.data());
        }
    }

    // ---- Main scan: enumerate mk via CSR, probe mc, read title ----
    std::string_view min_title;
    bool have_min = false;

    {
        GENDB_PHASE("main_scan");
        if (target_k_id >= 0) {
            int32_t lo = mk_csr_off[target_k_id];
            int32_t hi = mk_csr_off[target_k_id + 1];

            for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
                int32_t mk_row = mk_csr_rowids[k_pos];
                int32_t mv = mk_movie_id_bin[mk_row];

                // probe movie_companies
                int32_t mc_lo = mc_csr_off[mv];
                int32_t mc_hi = mc_csr_off[mv + 1];

                bool qualifies = false;
                for (int32_t r = mc_lo; r < mc_hi; ++r) {
                    int32_t cid = mc_company_id_bin[r];
                    if (cn_pass[cid]) { qualifies = true; break; }
                }
                if (!qualifies) continue;

                // read title: title.id is 1-based, so row index = mv - 1
                int64_t tlo = t_off[mv - 1];
                int64_t thi = t_off[mv];
                std::string_view title(t_dat + tlo, (size_t)(thi - tlo));
                if (!have_min || title < min_title) {
                    min_title = title;
                    have_min = true;
                }
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q2b.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(fp, "movie_title\n");
        if (have_min) {
            std::fwrite(min_title.data(), 1, min_title.size(), fp);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
