// Q15a: MIN(mi.info), MIN(t.title) with semi-joins to aka_title, movie_keyword,
// movie_companies (US + (200 + (worldwide)), movie_info (release_dates, internet, USA:% 200%)
// Title-driven, parallel morsel scan.

#define _GNU_SOURCE
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "timing_utils.h"
#include "cli_params.h"

static const void* mmap_file(const std::string& path, size_t& out_size) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st; fstat(fd, &st);
    out_size = (size_t)st.st_size;
    void* p = nullptr;
    if (out_size > 0) {
        p = mmap(nullptr, out_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    }
    ::close(fd);
    return p;
}

template <typename T>
static const T* mmap_as(const std::string& path, size_t& nelem) {
    size_t sz = 0;
    const void* p = mmap_file(path, sz);
    nelem = sz / sizeof(T);
    return reinterpret_cast<const T*>(p);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    GENDB_PHASE("total");

    // ------------ Resolve scalars ------------
    int16_t us_code = 0;
    int32_t it1_id = -1;
    size_t cn_n = 0;
    const int16_t* cn_cc = nullptr;
    const int32_t* t_pyear = nullptr;
    size_t t_n = 0;
    const int64_t* t_title_off = nullptr;
    const char* t_title_dat = nullptr;
    const int32_t* at_mid_off = nullptr;
    const int32_t* mk_mid_off = nullptr;
    const int32_t* mc_mid_off = nullptr;
    const int32_t* mi_mid_off = nullptr;
    size_t at_off_n = 0, mk_off_n = 0, mc_off_n = 0, mi_off_n = 0;
    const int32_t* mc_company_id = nullptr;
    const int64_t* mc_note_off = nullptr;
    const char* mc_note_dat = nullptr;
    size_t mc_n = 0;
    const int32_t* mi_info_type_id = nullptr;
    const int64_t* mi_info_off = nullptr;
    const char* mi_info_dat = nullptr;
    const int64_t* mi_note_off = nullptr;
    const char* mi_note_dat = nullptr;
    size_t mi_n = 0;

    {
        GENDB_PHASE("data_loading");

        // Resolve us_code from company_name.country_code dict
        {
            size_t off_n = 0, dat_n = 0;
            const int64_t* off = mmap_as<int64_t>(store + "/company_name/country_code.dict.off", off_n);
            const char* dat = (const char*)mmap_file(store + "/company_name/country_code.dict.dat", dat_n);
            for (size_t i = 0; i + 1 < off_n; ++i) {
                size_t len = (size_t)(off[i+1] - off[i]);
                if (len == 4 && std::memcmp(dat + off[i], "[us]", 4) == 0) {
                    us_code = (int16_t)(i + 1);
                    break;
                }
            }
            if (us_code == 0) { std::fprintf(stderr, "[us] not found\n"); return 1; }
        }
        cn_cc = mmap_as<int16_t>(store + "/company_name/country_code.bin", cn_n);

        // Resolve it1_id from info_type.info varlen
        {
            size_t off_n = 0, dat_n = 0, idn = 0;
            const int64_t* off = mmap_as<int64_t>(store + "/info_type/info.off", off_n);
            const char* dat = (const char*)mmap_file(store + "/info_type/info.dat", dat_n);
            const int32_t* ids = mmap_as<int32_t>(store + "/info_type/id.bin", idn);
            size_t nrows = off_n - 1;
            for (size_t i = 0; i < nrows; ++i) {
                size_t len = (size_t)(off[i+1] - off[i]);
                if (len == 13 && std::memcmp(dat + off[i], "release dates", 13) == 0) {
                    it1_id = ids[i];
                    break;
                }
            }
            if (it1_id < 0) { std::fprintf(stderr, "release dates not found\n"); return 1; }
        }

        // title columns
        t_pyear = mmap_as<int32_t>(store + "/title/production_year.bin", t_n);
        size_t toff_n = 0, tdat_n = 0;
        t_title_off = mmap_as<int64_t>(store + "/title/title.off", toff_n);
        t_title_dat = (const char*)mmap_file(store + "/title/title.dat", tdat_n);

        // offset indexes
        at_mid_off = mmap_as<int32_t>(store + "/_idx/aka_title__movie_id__offsets.bin", at_off_n);
        mk_mid_off = mmap_as<int32_t>(store + "/_idx/movie_keyword__movie_id__offsets.bin", mk_off_n);
        mc_mid_off = mmap_as<int32_t>(store + "/_idx/movie_companies__movie_id__offsets.bin", mc_off_n);
        mi_mid_off = mmap_as<int32_t>(store + "/_idx/movie_info__movie_id__offsets.bin", mi_off_n);

        // movie_companies
        mc_company_id = mmap_as<int32_t>(store + "/movie_companies/company_id.bin", mc_n);
        size_t mcoff_n = 0, mcdat_n = 0;
        mc_note_off = mmap_as<int64_t>(store + "/movie_companies/note.off", mcoff_n);
        mc_note_dat = (const char*)mmap_file(store + "/movie_companies/note.dat", mcdat_n);

        // movie_info
        mi_info_type_id = mmap_as<int32_t>(store + "/movie_info/info_type_id.bin", mi_n);
        size_t a = 0, b = 0, c = 0, d = 0;
        mi_info_off = mmap_as<int64_t>(store + "/movie_info/info.off", a);
        mi_info_dat = (const char*)mmap_file(store + "/movie_info/info.dat", b);
        mi_note_off = mmap_as<int64_t>(store + "/movie_info/note.off", c);
        mi_note_dat = (const char*)mmap_file(store + "/movie_info/note.dat", d);
    }

    // Build cn_us bitset (company_name dense PK; idx = company_id-1)
    std::vector<uint8_t> cn_us(cn_n, 0);
    {
        GENDB_PHASE("build_cn_us_bitset");
        for (size_t i = 0; i < cn_n; ++i) {
            cn_us[i] = (cn_cc[i] == us_code) ? 1 : 0;
        }
    }

    // ------------ Driver: title morsels in parallel ------------
    const int nthreads = (int)std::min<unsigned>(12, std::max(1u, std::thread::hardware_concurrency()));
    struct LocalMin {
        std::string mi_info;
        std::string title;
        bool has = false;
    };
    std::vector<LocalMin> locals(nthreads);

    auto worker = [&](int tid, int32_t r_lo, int32_t r_hi) {
        std::string& min_mi = locals[tid].mi_info;
        std::string& min_t  = locals[tid].title;
        bool& has = locals[tid].has;

        for (int32_t r = r_lo; r < r_hi; ++r) {
            int32_t py = t_pyear[r];
            if (py == INT32_MIN || py <= 2000) continue;

            int32_t mv = r + 1;

            // aka_title existence
            int32_t at_lo = at_mid_off[mv], at_hi = at_mid_off[mv + 1];
            if (at_lo == at_hi) continue;

            // movie_keyword existence
            int32_t mk_lo = mk_mid_off[mv], mk_hi = mk_mid_off[mv + 1];
            if (mk_lo == mk_hi) continue;

            // movie_companies: need at least one row with cn_us && note contains (200 and (worldwide)
            int32_t mc_lo = mc_mid_off[mv], mc_hi = mc_mid_off[mv + 1];
            bool mc_ok = false;
            for (int32_t i = mc_lo; i < mc_hi; ++i) {
                int32_t cid = mc_company_id[i];
                if (cid <= 0 || (size_t)cid > cn_n || !cn_us[cid - 1]) continue;
                const char* p = mc_note_dat + mc_note_off[i];
                size_t n = (size_t)(mc_note_off[i + 1] - mc_note_off[i]);
                if (n == 0) continue;
                if (!::memmem(p, n, "(200", 4)) continue;
                if (!::memmem(p, n, "(worldwide)", 11)) continue;
                mc_ok = true;
                break;
            }
            if (!mc_ok) continue;

            // movie_info: track min(mi.info) where info_type_id==it1_id, note has 'internet', info like 'USA:% 200%'
            int32_t mio_lo = mi_mid_off[mv], mio_hi = mi_mid_off[mv + 1];
            // Find lex-min info among surviving mi rows for this title
            const char* best_info_p = nullptr;
            size_t best_info_n = 0;
            for (int32_t i = mio_lo; i < mio_hi; ++i) {
                if (mi_info_type_id[i] != it1_id) continue;
                const char* np = mi_note_dat + mi_note_off[i];
                size_t nn = (size_t)(mi_note_off[i + 1] - mi_note_off[i]);
                if (nn < 8 || !::memmem(np, nn, "internet", 8)) continue;
                const char* ip = mi_info_dat + mi_info_off[i];
                size_t in = (size_t)(mi_info_off[i + 1] - mi_info_off[i]);
                if (in < 8) continue;
                if (std::memcmp(ip, "USA:", 4) != 0) continue;
                if (!::memmem(ip + 4, in - 4, " 200", 4)) continue;
                // Track lex-min mi.info for this title
                if (best_info_p == nullptr) {
                    best_info_p = ip; best_info_n = in;
                } else {
                    size_t mlen = std::min(best_info_n, in);
                    int cmp = std::memcmp(ip, best_info_p, mlen);
                    if (cmp < 0 || (cmp == 0 && in < best_info_n)) {
                        best_info_p = ip; best_info_n = in;
                    }
                }
            }
            if (best_info_p == nullptr) continue;

            // This title qualifies. Update locals.
            std::string this_info(best_info_p, best_info_n);
            const char* tp = t_title_dat + t_title_off[r];
            size_t tn = (size_t)(t_title_off[r + 1] - t_title_off[r]);
            std::string this_title(tp, tn);

            if (!has) {
                min_mi = std::move(this_info);
                min_t  = std::move(this_title);
                has = true;
            } else {
                if (this_info < min_mi) min_mi = std::move(this_info);
                if (this_title < min_t) min_t  = std::move(this_title);
            }
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        int32_t total = (int32_t)t_n;
        int32_t chunk = (total + nthreads - 1) / nthreads;
        for (int t = 0; t < nthreads; ++t) {
            int32_t lo = t * chunk;
            int32_t hi = std::min(total, lo + chunk);
            if (lo >= hi) continue;
            threads.emplace_back(worker, t, lo, hi);
        }
        for (auto& th : threads) th.join();
    }

    // Reduce
    std::string final_mi_info;
    std::string final_title;
    bool found = false;
    for (auto& lm : locals) {
        if (!lm.has) continue;
        if (!found) {
            final_mi_info = lm.mi_info;
            final_title   = lm.title;
            found = true;
        } else {
            if (lm.mi_info < final_mi_info) final_mi_info = lm.mi_info;
            if (lm.title   < final_title)   final_title   = lm.title;
        }
    }

    // ------------ Output ------------
    {
        GENDB_PHASE("output");
        ::mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q15a.csv";
        std::ofstream out(out_path);
        out << "release_date,internet_movie\n";
        if (found) {
            out << final_mi_info << "," << final_title << "\n";
        } else {
            out << ",\n";
        }
        out.close();
    }

    return 0;
}
