// Q1c — IMDB JOB
// SELECT MIN(mc.note), MIN(t.title), MIN(t.production_year)
// FROM company_type ct, info_type it, movie_companies mc, movie_info_idx mi_idx, title t
// WHERE ct.kind='production companies' AND it.info='top 250 rank'
//   AND mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%'
//   AND mc.note LIKE '%(co-production)%'
//   AND t.production_year > 2010
//   AND ct.id = mc.company_type_id
//   AND t.id = mc.movie_id AND t.id = mi_idx.movie_id
//   AND it.id = mi_idx.info_type_id;

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <climits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <omp.h>

#include "timing_utils.h"

namespace fs = std::filesystem;

struct MappedFile {
    void* data = nullptr;
    size_t size = 0;
    int fd = -1;
};

static MappedFile map_file(const std::string& path) {
    MappedFile m;
    m.fd = open(path.c_str(), O_RDONLY);
    if (m.fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(m.fd, &st) < 0) { std::fprintf(stderr, "stat failed: %s\n", path.c_str()); std::exit(1); }
    m.size = (size_t)st.st_size;
    if (m.size == 0) { m.data = nullptr; return m; }
    m.data = mmap(nullptr, m.size, PROT_READ, MAP_SHARED, m.fd, 0);
    if (m.data == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    madvise(m.data, m.size, MADV_WILLNEED);
    return m;
}

// Resolve target id from dim varlen column with id.bin (dense). Returns -1 if not found.
static int32_t resolve_dim_id(const std::string& dir, const std::string& col,
                              const char* target, size_t target_len) {
    MappedFile id  = map_file(dir + "/id.bin");
    MappedFile off = map_file(dir + "/" + col + ".off");
    MappedFile dat = map_file(dir + "/" + col + ".dat");
    const int32_t* id_ptr  = (const int32_t*)id.data;
    const int64_t* off_ptr = (const int64_t*)off.data;
    const char*   dat_ptr  = (const char*)dat.data;
    size_t n = id.size / 4;
    int32_t result = -1;
    for (size_t r = 0; r < n; ++r) {
        int64_t lo = off_ptr[r], hi = off_ptr[r+1];
        int64_t len = hi - lo;
        if (len == (int64_t)target_len && std::memcmp(dat_ptr + lo, target, target_len) == 0) {
            result = id_ptr[r];
            break;
        }
    }
    return result;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) { std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]); return 1; }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // ---- Phase: resolve dimension targets ----
    int32_t target_ct_id, target_it_id;
    {
        GENDB_PHASE("resolve_dims");
        const char ct_target[] = "production companies";
        const char it_target[] = "top 250 rank";
        target_ct_id = resolve_dim_id(gendb_dir + "/company_type", "kind", ct_target, sizeof(ct_target)-1);
        target_it_id = resolve_dim_id(gendb_dir + "/info_type",   "info", it_target, sizeof(it_target)-1);
        if (target_ct_id < 0 || target_it_id < 0) {
            std::fprintf(stderr, "Dim resolution failed: ct=%d it=%d\n", target_ct_id, target_it_id);
            return 1;
        }
    }

    // ---- Phase: data_loading ----
    MappedFile t_id, t_year, t_title_off, t_title_dat;
    MappedFile mc_movie_id, mc_ct, mc_note_off, mc_note_dat;
    MappedFile mi_movie_id, mi_it;
    MappedFile mc_idx_off, mi_idx_off;
    {
        GENDB_PHASE("data_loading");
        t_id        = map_file(gendb_dir + "/title/id.bin");
        t_year      = map_file(gendb_dir + "/title/production_year.bin");
        t_title_off = map_file(gendb_dir + "/title/title.off");
        t_title_dat = map_file(gendb_dir + "/title/title.dat");
        mc_movie_id = map_file(gendb_dir + "/movie_companies/movie_id.bin");
        mc_ct       = map_file(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note_off = map_file(gendb_dir + "/movie_companies/note.off");
        mc_note_dat = map_file(gendb_dir + "/movie_companies/note.dat");
        mi_movie_id = map_file(gendb_dir + "/movie_info_idx/movie_id.bin");
        mi_it       = map_file(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mc_idx_off  = map_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_idx_off  = map_file(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
    }

    const int32_t* t_id_ptr     = (const int32_t*)t_id.data;
    const int32_t* t_year_ptr   = (const int32_t*)t_year.data;
    const int64_t* t_toff       = (const int64_t*)t_title_off.data;
    const char*    t_tdat       = (const char*)   t_title_dat.data;
    const int32_t* mc_mid_ptr   = (const int32_t*)mc_movie_id.data;
    const int32_t* mc_ct_ptr    = (const int32_t*)mc_ct.data;
    const int64_t* mc_noff      = (const int64_t*)mc_note_off.data;
    const char*    mc_ndat      = (const char*)   mc_note_dat.data;
    const int32_t* mi_mid_ptr   = (const int32_t*)mi_movie_id.data;
    const int32_t* mi_it_ptr    = (const int32_t*)mi_it.data;
    const int32_t* mc_idx       = (const int32_t*)mc_idx_off.data;
    const int32_t* mi_idx       = (const int32_t*)mi_idx_off.data;
    const size_t   n_title      = t_id.size / 4;
    (void)mc_mid_ptr; (void)mi_mid_ptr;

    // Patterns
    static const char POS_PAT[] = "(co-production)";
    static const size_t POS_LEN = sizeof(POS_PAT) - 1; // 15
    static const char NEG_PAT[] = "(as Metro-Goldwyn-Mayer Pictures)";
    static const size_t NEG_LEN = sizeof(NEG_PAT) - 1;

    struct LocalAgg {
        bool has = false;
        // MIN(note) — point to mc.note.dat
        const char* note_ptr = nullptr; int64_t note_len = 0;
        const char* title_ptr = nullptr; int64_t title_len = 0;
        int32_t year = INT32_MAX;
    };

    auto cmp_lt = [](const char* a, int64_t alen, const char* b, int64_t blen) -> bool {
        int64_t n = (alen < blen) ? alen : blen;
        int c = std::memcmp(a, b, n);
        if (c != 0) return c < 0;
        return alen < blen;
    };

    int nthreads = omp_get_max_threads();
    std::vector<LocalAgg> locals(nthreads);

    // ---- Phase: main_scan ----
    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            LocalAgg& L = locals[tid];

            #pragma omp for schedule(static, 4096)
            for (size_t r = 0; r < n_title; ++r) {
                int32_t y = t_year_ptr[r];
                if (y == INT32_MIN || y <= 2010) continue;
                int32_t v = t_id_ptr[r];

                // Probe mi_idx first (extremely selective filter)
                int32_t mi_lo = mi_idx[v], mi_hi = mi_idx[v+1];
                bool mi_match = false;
                for (int32_t i = mi_lo; i < mi_hi; ++i) {
                    if (mi_it_ptr[i] == target_it_id) { mi_match = true; break; }
                }
                if (!mi_match) continue;

                // Probe mc
                int32_t mc_lo = mc_idx[v], mc_hi = mc_idx[v+1];
                for (int32_t i = mc_lo; i < mc_hi; ++i) {
                    if (mc_ct_ptr[i] != target_ct_id) continue;
                    int64_t no_lo = mc_noff[i], no_hi = mc_noff[i+1];
                    int64_t nlen = no_hi - no_lo;
                    if (nlen < (int64_t)POS_LEN) continue; // empty / too short
                    const char* nptr = mc_ndat + no_lo;
                    // positive LIKE
                    if (!memmem(nptr, nlen, POS_PAT, POS_LEN)) continue;
                    // negative NOT LIKE
                    if (nlen >= (int64_t)NEG_LEN && memmem(nptr, nlen, NEG_PAT, NEG_LEN)) continue;

                    // surviving row
                    int64_t ti_lo = t_toff[r], ti_hi = t_toff[r+1];
                    int64_t tlen  = ti_hi - ti_lo;
                    const char* tptr = t_tdat + ti_lo;

                    if (!L.has) {
                        L.has = true;
                        L.note_ptr = nptr; L.note_len = nlen;
                        L.title_ptr = tptr; L.title_len = tlen;
                        L.year = y;
                    } else {
                        if (cmp_lt(nptr, nlen, L.note_ptr, L.note_len)) {
                            L.note_ptr = nptr; L.note_len = nlen;
                        }
                        if (cmp_lt(tptr, tlen, L.title_ptr, L.title_len)) {
                            L.title_ptr = tptr; L.title_len = tlen;
                        }
                        if (y < L.year) L.year = y;
                    }
                }
            }
        }
    }

    // ---- Phase: reduce + output ----
    LocalAgg G;
    for (const auto& L : locals) {
        if (!L.has) continue;
        if (!G.has) { G = L; continue; }
        if (cmp_lt(L.note_ptr, L.note_len, G.note_ptr, G.note_len)) {
            G.note_ptr = L.note_ptr; G.note_len = L.note_len;
        }
        if (cmp_lt(L.title_ptr, L.title_len, G.title_ptr, G.title_len)) {
            G.title_ptr = L.title_ptr; G.title_len = L.title_len;
        }
        if (L.year < G.year) G.year = L.year;
    }

    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q1c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Failed to open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "production_note,movie_title,movie_year\n");
        if (G.has) {
            std::fwrite(G.note_ptr, 1, G.note_len, f);
            std::fputc(',', f);
            std::fwrite(G.title_ptr, 1, G.title_len, f);
            std::fprintf(f, ",%d\n", G.year);
        }
        std::fclose(f);
    }

    return 0;
}
