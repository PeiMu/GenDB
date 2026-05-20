// Q5b: MIN(t.title) for production companies with VHS/USA/1994 note,
// movie_info IN ('USA','America'), production_year > 2010.
//
// Pipeline: title (driver, production_year filter) → mc range probe → mi range probe.
// Aggregation: scalar lex-MIN of t.title.
//
// Parallelism: OpenMP morsel-driven over title rows.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <climits>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <omp.h>

#include "timing_utils.h"

namespace {

struct Mapped {
    const void* base = nullptr;
    size_t bytes = 0;
    int fd = -1;
};

static Mapped map_file(const std::string& path) {
    Mapped m;
    m.fd = ::open(path.c_str(), O_RDONLY);
    if (m.fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", path.c_str());
        std::exit(1);
    }
    struct stat st;
    if (fstat(m.fd, &st) < 0) {
        std::fprintf(stderr, "stat failed: %s\n", path.c_str());
        std::exit(1);
    }
    m.bytes = st.st_size;
    if (m.bytes == 0) return m;
    void* p = mmap(nullptr, m.bytes, PROT_READ, MAP_PRIVATE, m.fd, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed: %s\n", path.c_str());
        std::exit(1);
    }
    m.base = p;
    madvise(p, m.bytes, MADV_SEQUENTIAL);
    return m;
}

} // namespace

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];

    // -------------------------------------------------------------------
    // Data loading
    // -------------------------------------------------------------------
    Mapped ct_kind_off, ct_kind_dat;
    Mapped t_year, t_title_off, t_title_dat;
    Mapped mc_off_idx, mi_off_idx;
    Mapped mc_ct, mc_note_off, mc_note_dat;
    Mapped mi_info_off, mi_info_dat;

    size_t n_title = 0;
    size_t n_mc = 0;
    size_t n_mi = 0;

    int32_t target_ct_id = -1;

    {
        GENDB_PHASE("data_loading");
        ct_kind_off    = map_file(gendb + "/company_type/kind.off");
        ct_kind_dat    = map_file(gendb + "/company_type/kind.dat");

        t_year         = map_file(gendb + "/title/production_year.bin");
        t_title_off    = map_file(gendb + "/title/title.off");
        t_title_dat    = map_file(gendb + "/title/title.dat");

        mc_off_idx     = map_file(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off_idx     = map_file(gendb + "/_idx/movie_info__movie_id__offsets.bin");

        mc_ct          = map_file(gendb + "/movie_companies/company_type_id.bin");
        mc_note_off    = map_file(gendb + "/movie_companies/note.off");
        mc_note_dat    = map_file(gendb + "/movie_companies/note.dat");

        mi_info_off    = map_file(gendb + "/movie_info/info.off");
        mi_info_dat    = map_file(gendb + "/movie_info/info.dat");

        n_title = t_year.bytes / sizeof(int32_t);
        n_mc    = mc_ct.bytes / sizeof(int32_t);
        n_mi    = mi_info_off.bytes / sizeof(uint64_t) - 1;

        // Resolve target_ct_id by scanning company_type.kind varlen.
        const uint64_t* ck_off = static_cast<const uint64_t*>(ct_kind_off.base);
        const char* ck_dat = static_cast<const char*>(ct_kind_dat.base);
        size_t n_ct = ct_kind_off.bytes / sizeof(uint64_t) - 1;
        static const char NEEDLE[] = "production companies";
        static const size_t NEEDLE_LEN = sizeof(NEEDLE) - 1;
        for (size_t r = 0; r < n_ct; ++r) {
            uint64_t lo = ck_off[r], hi = ck_off[r + 1];
            size_t len = hi - lo;
            if (len == NEEDLE_LEN && std::memcmp(ck_dat + lo, NEEDLE, NEEDLE_LEN) == 0) {
                target_ct_id = static_cast<int32_t>(r + 1);
                break;
            }
        }
        if (target_ct_id < 0) {
            std::fprintf(stderr, "could not resolve company_type 'production companies'\n");
            return 2;
        }
    }

    // -------------------------------------------------------------------
    // Main scan: parallel over title rows.
    // -------------------------------------------------------------------
    const int32_t* py = static_cast<const int32_t*>(t_year.base);
    const uint64_t* t_off = static_cast<const uint64_t*>(t_title_off.base);
    const char*     t_dat = static_cast<const char*>(t_title_dat.base);
    const int32_t* mc_off = static_cast<const int32_t*>(mc_off_idx.base);
    const int32_t* mi_off = static_cast<const int32_t*>(mi_off_idx.base);
    const int32_t* mc_ctid = static_cast<const int32_t*>(mc_ct.base);
    const uint64_t* mc_noff = static_cast<const uint64_t*>(mc_note_off.base);
    const char*     mc_ndat = static_cast<const char*>(mc_note_dat.base);
    const uint64_t* mi_ioff = static_cast<const uint64_t*>(mi_info_off.base);
    const char*     mi_idat = static_cast<const char*>(mi_info_dat.base);

    std::string global_min;
    bool global_has = false;

    {
        GENDB_PHASE("main_scan");

        int nthreads = omp_get_max_threads();
        std::vector<std::string> local_min(nthreads);
        std::vector<char> local_has(nthreads, 0);

        const size_t MORSEL = 50000;
        const size_t num_morsels = (n_title + MORSEL - 1) / MORSEL;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string& my_min = local_min[tid];
            bool my_has = false;

            #pragma omp for schedule(dynamic, 1) nowait
            for (size_t m = 0; m < num_morsels; ++m) {
                size_t r_lo = m * MORSEL;
                size_t r_hi = r_lo + MORSEL;
                if (r_hi > n_title) r_hi = n_title;

                for (size_t r = r_lo; r < r_hi; ++r) {
                    int32_t year = py[r];
                    if (year == INT32_MIN || year <= 2010) continue;

                    int32_t t_id = static_cast<int32_t>(r + 1);

                    // mc range probe — first qualifying mc row short-circuits.
                    int32_t mc_lo = mc_off[t_id];
                    int32_t mc_hi = mc_off[t_id + 1];
                    if (mc_lo == mc_hi) continue;

                    bool mc_ok = false;
                    for (int32_t mr = mc_lo; mr < mc_hi; ++mr) {
                        if (mc_ctid[mr] != target_ct_id) continue;
                        uint64_t nlo = mc_noff[mr];
                        uint64_t nhi = mc_noff[mr + 1];
                        size_t nlen = nhi - nlo;
                        if (nlen < 17) continue;
                        const char* note = mc_ndat + nlo;
                        // Cheapest/most-selective first: '(VHS)', then '(1994)', then '(USA)'.
                        if (!memmem(note, nlen, "(VHS)", 5))    continue;
                        if (!memmem(note, nlen, "(1994)", 6))   continue;
                        if (!memmem(note, nlen, "(USA)", 5))    continue;
                        mc_ok = true;
                        break;
                    }
                    if (!mc_ok) continue;

                    // mi range probe — first qualifying row short-circuits.
                    int32_t mi_lo = mi_off[t_id];
                    int32_t mi_hi = mi_off[t_id + 1];
                    if (mi_lo == mi_hi) continue;

                    bool mi_ok = false;
                    for (int32_t ir = mi_lo; ir < mi_hi; ++ir) {
                        uint64_t ilo = mi_ioff[ir];
                        uint64_t ihi = mi_ioff[ir + 1];
                        size_t ilen = ihi - ilo;
                        const char* info = mi_idat + ilo;
                        if (ilen == 3) {
                            if (info[0] == 'U' && info[1] == 'S' && info[2] == 'A') {
                                mi_ok = true; break;
                            }
                        } else if (ilen == 7) {
                            if (std::memcmp(info, "America", 7) == 0) {
                                mi_ok = true; break;
                            }
                        }
                    }
                    if (!mi_ok) continue;

                    // Both passed — fetch title and update local MIN.
                    uint64_t tlo = t_off[r];
                    uint64_t thi = t_off[r + 1];
                    size_t tlen = thi - tlo;
                    const char* tdat = t_dat + tlo;

                    if (!my_has) {
                        my_min.assign(tdat, tlen);
                        my_has = true;
                    } else {
                        // lex-min comparison
                        size_t cmplen = (tlen < my_min.size()) ? tlen : my_min.size();
                        int c = std::memcmp(tdat, my_min.data(), cmplen);
                        if (c < 0 || (c == 0 && tlen < my_min.size())) {
                            my_min.assign(tdat, tlen);
                        }
                    }
                }
            }

            local_has[tid] = my_has ? 1 : 0;
        }

        // Final reduction across threads.
        for (int t = 0; t < nthreads; ++t) {
            if (!local_has[t]) continue;
            if (!global_has) {
                global_min = std::move(local_min[t]);
                global_has = true;
            } else {
                const std::string& s = local_min[t];
                size_t cmplen = (s.size() < global_min.size()) ? s.size() : global_min.size();
                int c = std::memcmp(s.data(), global_min.data(), cmplen);
                if (c < 0 || (c == 0 && s.size() < global_min.size())) {
                    global_min = s;
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // Output: single-column CSV; empty value when no match (SQL NULL).
    // -------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q5b.csv";
        FILE* f = std::fopen(outpath.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", outpath.c_str());
            return 3;
        }
        std::fprintf(f, "american_vhs_movie\n");
        if (global_has) {
            // CSV-escape: if title contains comma/quote/newline, wrap in quotes.
            bool needs_quote = false;
            for (char c : global_min) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
            }
            if (needs_quote) {
                std::fputc('"', f);
                for (char c : global_min) {
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
                std::fputc('\n', f);
            } else {
                std::fwrite(global_min.data(), 1, global_min.size(), f);
                std::fputc('\n', f);
            }
        } else {
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }

    return 0;
}
