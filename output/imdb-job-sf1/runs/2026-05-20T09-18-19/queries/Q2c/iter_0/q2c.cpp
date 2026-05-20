// Q2c: SELECT MIN(t.title) FROM cn,k,mc,mk,t
//      WHERE cn.country_code='[sm]' AND k.keyword='character-name-in-title'
//      AND cn.id=mc.company_id AND mc.movie_id=t.id=mk.movie_id AND mk.keyword_id=k.id

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
#include <sys/types.h>
#include <fstream>
#include <filesystem>

#include "timing_utils.h"

namespace {

struct Mapped {
    void*  data = nullptr;
    size_t size = 0;
};

Mapped mmap_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open %s failed\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (::fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat failed\n"); std::exit(1); }
    void* p = nullptr;
    if (st.st_size > 0) {
        p = ::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { std::fprintf(stderr, "mmap %s failed\n", path.c_str()); std::exit(1); }
    }
    ::close(fd);
    return {p, (size_t)st.st_size};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store   = argv[1];
    std::string results = argv[2];

    GENDB_PHASE("total");

    // ---- data load (mmap) ----
    Mapped m_cn_cc, m_cc_off, m_cc_dat;
    Mapped m_k_off, m_k_dat;
    Mapped m_mc_movie, m_mc_company;
    Mapped m_mk_movie, m_mk_kid;
    Mapped m_t_off, m_t_dat;
    Mapped m_mk_kid_off, m_mk_kid_rowids;
    Mapped m_mc_mid_off;

    {
        GENDB_PHASE("data_loading");
        m_cn_cc        = mmap_file(store + "/company_name/country_code.bin");
        m_cc_off       = mmap_file(store + "/company_name/country_code.dict.off");
        m_cc_dat       = mmap_file(store + "/company_name/country_code.dict.dat");

        m_k_off        = mmap_file(store + "/keyword/keyword.off");
        m_k_dat        = mmap_file(store + "/keyword/keyword.dat");

        m_mc_movie     = mmap_file(store + "/movie_companies/movie_id.bin");
        m_mc_company   = mmap_file(store + "/movie_companies/company_id.bin");

        m_mk_movie     = mmap_file(store + "/movie_keyword/movie_id.bin");
        m_mk_kid       = mmap_file(store + "/movie_keyword/keyword_id.bin");

        m_t_off        = mmap_file(store + "/title/title.off");
        m_t_dat        = mmap_file(store + "/title/title.dat");

        m_mk_kid_off     = mmap_file(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        m_mk_kid_rowids  = mmap_file(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        m_mc_mid_off     = mmap_file(store + "/_idx/movie_companies__movie_id__offsets.bin");
    }

    // ---- dict resolution: target country_code for '[sm]' ----
    int16_t target_code = 0;
    {
        GENDB_PHASE("resolve_country_code");
        const int64_t* off = reinterpret_cast<const int64_t*>(m_cc_off.data);
        size_t off_n = m_cc_off.size / sizeof(int64_t);
        const char* dat = reinterpret_cast<const char*>(m_cc_dat.data);
        for (size_t i = 0; i + 1 < off_n; ++i) {
            int64_t a = off[i], b = off[i + 1];
            std::string_view s(dat + a, (size_t)(b - a));
            if (s == "[sm]") { target_code = (int16_t)(i + 1); break; }
        }
    }

    // ---- keyword resolution: target_k_id for 'character-name-in-title' ----
    int32_t target_k_id = -1;
    const size_t keyword_rows = 134170;
    {
        GENDB_PHASE("resolve_keyword");
        const int64_t* off = reinterpret_cast<const int64_t*>(m_k_off.data);
        const char* dat = reinterpret_cast<const char*>(m_k_dat.data);
        const std::string_view target("character-name-in-title");
        for (size_t i = 0; i < keyword_rows; ++i) {
            int64_t a = off[i], b = off[i + 1];
            std::string_view s(dat + a, (size_t)(b - a));
            if (s == target) {
                // keyword.id is dense identity → id = i + 1
                target_k_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    std::string best_title;
    bool have_best = false;

    if (target_code != 0 && target_k_id > 0) {

        // ---- Build cn_pass bitmap over cn.id 1..234997 ----
        // bit i corresponds to cn.id == (i+1).
        const size_t cn_rows = 234997;
        std::vector<uint64_t> cn_pass((cn_rows + 64) / 64, 0ull);
        {
            GENDB_PHASE("cn_build_bitmap");
            const int16_t* cc = reinterpret_cast<const int16_t*>(m_cn_cc.data);
            for (size_t i = 0; i < cn_rows; ++i) {
                if (cc[i] == target_code) {
                    uint32_t cn_id = (uint32_t)(i + 1);
                    cn_pass[cn_id >> 6] |= (1ull << (cn_id & 63));
                }
            }
        }

        // ---- Probe mk CSR for target_k_id; for each candidate movie_id probe mc ----
        const int32_t* mk_off       = reinterpret_cast<const int32_t*>(m_mk_kid_off.data);
        const int32_t* mk_rowids    = reinterpret_cast<const int32_t*>(m_mk_kid_rowids.data);
        const int32_t* mk_movie_id  = reinterpret_cast<const int32_t*>(m_mk_movie.data);
        const int32_t* mc_off       = reinterpret_cast<const int32_t*>(m_mc_mid_off.data);
        const int32_t* mc_company   = reinterpret_cast<const int32_t*>(m_mc_company.data);
        const int64_t* t_off        = reinterpret_cast<const int64_t*>(m_t_off.data);
        const char*    t_dat        = reinterpret_cast<const char*>(m_t_dat.data);

        int32_t lo = mk_off[target_k_id];
        int32_t hi = mk_off[target_k_id + 1];

        {
            GENDB_PHASE("main_scan");
            for (int32_t kp = lo; kp < hi; ++kp) {
                int32_t mk_row = mk_rowids[kp];
                int32_t mv = mk_movie_id[mk_row];
                if (mv <= 0) continue;

                // probe mc for this movie_id
                int32_t mlo = mc_off[mv];
                int32_t mhi = mc_off[mv + 1];
                bool pass = false;
                for (int32_t r = mlo; r < mhi; ++r) {
                    uint32_t cid = (uint32_t)mc_company[r];
                    if (cid == 0) continue;
                    if (cn_pass[cid >> 6] & (1ull << (cid & 63))) {
                        pass = true;
                        break;
                    }
                }
                if (!pass) continue;

                // fetch title by mv (title.id is dense identity; row = mv-1)
                size_t trow = (size_t)(mv - 1);
                int64_t a = t_off[trow];
                int64_t b = t_off[trow + 1];
                std::string_view title(t_dat + a, (size_t)(b - a));
                if (!have_best || title < std::string_view(best_title)) {
                    best_title.assign(title);
                    have_best = true;
                }
            }
        }
    }

    // ---- output ----
    {
        GENDB_PHASE("output");
        std::filesystem::create_directories(results);
        std::string out_path = results + "/Q2c.csv";
        std::ofstream out(out_path);
        out << "movie_title\n";
        if (have_best) {
            out << best_title << "\n";
        } else {
            out << "\n";
        }
    }

    return 0;
}
