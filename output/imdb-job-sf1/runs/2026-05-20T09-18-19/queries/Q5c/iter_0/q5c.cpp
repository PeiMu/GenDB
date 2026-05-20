// Q5c: MIN(t.title) where production_year>1990, mc.note has "(USA)" but not "(TV)",
//      mc.company_type_id == production_companies, mi.info IN small set.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>
#include "timing_utils.h"

using namespace gendb;

struct Mapped {
    const void* ptr = nullptr;
    size_t size = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open failed: %s\n", path.c_str()); exit(1); }
    struct stat st;
    if (fstat(fd, &st) < 0) { fprintf(stderr, "fstat failed: %s\n", path.c_str()); exit(1); }
    void* p = nullptr;
    if (st.st_size > 0) {
        p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { fprintf(stderr, "mmap failed: %s\n", path.c_str()); exit(1); }
        madvise(p, st.st_size, MADV_WILLNEED);
    }
    close(fd);
    return {p, (size_t)st.st_size};
}

// Check whether [hay, hay+hay_len) contains the literal needle (short, no nulls).
static inline bool contains(const char* hay, int hay_len, const char* needle, int needle_len) {
    if (hay_len < needle_len) return false;
    int last = hay_len - needle_len;
    for (int i = 0; i <= last; ++i) {
        if (hay[i] == needle[0] && std::memcmp(hay + i, needle, needle_len) == 0) return true;
    }
    return false;
}

// 10-literal IN set membership for movie_info.info:
//   'Sweden'(6),'Norway'(6),'Germany'(7),'Denmark'(7),'Swedish'(7),
//   'Denish'(6),'Norwegian'(9),'German'(6),'USA'(3),'American'(8)
static inline bool info_in_set(const char* p, int len) {
    switch (len) {
        case 3:
            return p[0]=='U' && p[1]=='S' && p[2]=='A';
        case 6:
            // Sweden, Norway, Denish, German
            if (p[0]=='S') return std::memcmp(p, "Sweden", 6)==0;
            if (p[0]=='N') return std::memcmp(p, "Norway", 6)==0;
            if (p[0]=='D') return std::memcmp(p, "Denish", 6)==0;
            if (p[0]=='G') return std::memcmp(p, "German", 6)==0;
            return false;
        case 7:
            // Germany, Denmark, Swedish
            if (p[0]=='G') return std::memcmp(p, "Germany", 7)==0;
            if (p[0]=='D') return std::memcmp(p, "Denmark", 7)==0;
            if (p[0]=='S') return std::memcmp(p, "Swedish", 7)==0;
            return false;
        case 8:
            return std::memcmp(p, "American", 8)==0;
        case 9:
            return std::memcmp(p, "Norwegian", 9)==0;
        default:
            return false;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    GENDB_PHASE("total");

    // --- Resolve target_ct_id ---
    int32_t target_ct_id = -1;
    {
        GENDB_PHASE("resolve_ct");
        Mapped ct_off = map_file(gendb_dir + "/company_type/kind.off");
        Mapped ct_dat = map_file(gendb_dir + "/company_type/kind.dat");
        Mapped ct_id  = map_file(gendb_dir + "/company_type/id.bin");
        const int64_t* off = (const int64_t*)ct_off.ptr;
        const char* dat = (const char*)ct_dat.ptr;
        const int32_t* idv = (const int32_t*)ct_id.ptr;
        int64_t n = (int64_t)(ct_off.size / 8) - 1;
        const char* target = "production companies";
        int tlen = 20;
        for (int64_t r = 0; r < n; ++r) {
            int64_t lo = off[r], hi = off[r+1];
            int len = (int)(hi - lo);
            if (len == tlen && std::memcmp(dat + lo, target, tlen) == 0) {
                target_ct_id = idv[r];
                break;
            }
        }
        if (target_ct_id < 0) {
            fprintf(stderr, "target_ct_id not found\n");
            return 1;
        }
    }

    // --- mmap all data ---
    Mapped t_year_m, t_off_m, t_dat_m;
    Mapped mc_off_idx_m, mc_ct_m, mc_note_off_m, mc_note_dat_m;
    Mapped mi_off_idx_m, mi_info_off_m, mi_info_dat_m;
    int64_t num_titles = 0;
    {
        GENDB_PHASE("data_loading");
        t_year_m       = map_file(gendb_dir + "/title/production_year.bin");
        t_off_m        = map_file(gendb_dir + "/title/title.off");
        t_dat_m        = map_file(gendb_dir + "/title/title.dat");

        mc_off_idx_m   = map_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_ct_m        = map_file(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note_off_m  = map_file(gendb_dir + "/movie_companies/note.off");
        mc_note_dat_m  = map_file(gendb_dir + "/movie_companies/note.dat");

        mi_off_idx_m   = map_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off_m  = map_file(gendb_dir + "/movie_info/info.off");
        mi_info_dat_m  = map_file(gendb_dir + "/movie_info/info.dat");

        num_titles = (int64_t)(t_year_m.size / 4);
    }

    const int32_t* t_year      = (const int32_t*)t_year_m.ptr;
    const int64_t* t_off       = (const int64_t*)t_off_m.ptr;
    const char*    t_dat       = (const char*)t_dat_m.ptr;

    const int32_t* mc_off_idx  = (const int32_t*)mc_off_idx_m.ptr;
    const int32_t* mc_ct       = (const int32_t*)mc_ct_m.ptr;
    const int64_t* mc_note_off = (const int64_t*)mc_note_off_m.ptr;
    const char*    mc_note_dat = (const char*)mc_note_dat_m.ptr;

    const int32_t* mi_off_idx  = (const int32_t*)mi_off_idx_m.ptr;
    const int64_t* mi_info_off = (const int64_t*)mi_info_off_m.ptr;
    const char*    mi_info_dat = (const char*)mi_info_dat_m.ptr;

    // --- Main scan ---
    std::string best_title;
    bool have_best = false;
    {
        GENDB_PHASE("main_scan");

        int nthreads = omp_get_max_threads();
        std::vector<std::string> local_best(nthreads);
        std::vector<char> local_have(nthreads, 0);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string lb;
            bool lhave = false;
            const int32_t TARGET_CT = target_ct_id;

            #pragma omp for schedule(dynamic, 16384) nowait
            for (int64_t r = 0; r < num_titles; ++r) {
                int32_t y = t_year[r];
                if (y <= 1990 || y == INT32_MIN) continue;

                int32_t t_id = (int32_t)(r + 1);

                // Probe mc range
                int32_t mc_lo = mc_off_idx[t_id];
                int32_t mc_hi = mc_off_idx[t_id + 1];
                if (mc_lo >= mc_hi) continue;

                bool mc_ok = false;
                for (int32_t mr = mc_lo; mr < mc_hi; ++mr) {
                    if (mc_ct[mr] != TARGET_CT) continue;
                    int64_t no_lo = mc_note_off[mr];
                    int64_t no_hi = mc_note_off[mr + 1];
                    int nlen = (int)(no_hi - no_lo);
                    if (nlen < 5) continue;
                    const char* note = mc_note_dat + no_lo;
                    if (!contains(note, nlen, "(USA)", 5)) continue;
                    if (contains(note, nlen, "(TV)", 4)) continue;
                    mc_ok = true;
                    break;
                }
                if (!mc_ok) continue;

                // Probe mi range
                int32_t mi_lo = mi_off_idx[t_id];
                int32_t mi_hi = mi_off_idx[t_id + 1];
                if (mi_lo >= mi_hi) continue;

                bool mi_ok = false;
                for (int32_t ir = mi_lo; ir < mi_hi; ++ir) {
                    int64_t io_lo = mi_info_off[ir];
                    int64_t io_hi = mi_info_off[ir + 1];
                    int ilen = (int)(io_hi - io_lo);
                    if (info_in_set(mi_info_dat + io_lo, ilen)) {
                        mi_ok = true;
                        break;
                    }
                }
                if (!mi_ok) continue;

                // Candidate: read title
                int64_t to_lo = t_off[r];
                int64_t to_hi = t_off[r + 1];
                int tlen = (int)(to_hi - to_lo);
                const char* tptr = t_dat + to_lo;

                if (!lhave) {
                    lb.assign(tptr, tlen);
                    lhave = true;
                } else {
                    // lexicographic compare
                    int cmplen = std::min((int)lb.size(), tlen);
                    int c = std::memcmp(tptr, lb.data(), cmplen);
                    if (c < 0 || (c == 0 && tlen < (int)lb.size())) {
                        lb.assign(tptr, tlen);
                    }
                }
            }

            local_best[tid] = std::move(lb);
            local_have[tid] = lhave ? 1 : 0;
        }

        for (int i = 0; i < nthreads; ++i) {
            if (!local_have[i]) continue;
            if (!have_best) { best_title = local_best[i]; have_best = true; }
            else if (local_best[i] < best_title) best_title = local_best[i];
        }
    }

    // --- Write output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q5c.csv";
        // ensure results dir exists
        std::string mkdir_cmd = "mkdir -p '" + results_dir + "'";
        int rc = std::system(mkdir_cmd.c_str());
        (void)rc;
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { fprintf(stderr, "open output failed\n"); return 1; }
        std::fprintf(f, "american_movie\n");
        if (have_best) {
            // CSV-quote if value contains comma, quote, or newline
            bool need_quote = false;
            for (char c : best_title) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (need_quote) {
                std::fputc('"', f);
                for (char c : best_title) {
                    if (c == '"') { std::fputc('"', f); std::fputc('"', f); }
                    else std::fputc(c, f);
                }
                std::fputc('"', f);
                std::fputc('\n', f);
            } else {
                std::fprintf(f, "%s\n", best_title.c_str());
            }
        } else {
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }

    return 0;
}
