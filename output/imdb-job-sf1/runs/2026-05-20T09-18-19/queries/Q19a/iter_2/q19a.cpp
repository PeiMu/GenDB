// Q19a — MIN(n.name), MIN(t.title)
// Plan: scan title for production_year in [2005,2009], for each surviving
// movie probe mc/mi/ci ranges via offsets-only indexes, semi-join aka_name.
//
// Build:
//   g++ -O3 -march=native -std=c++17 -Wall -lpthread -fopenmp -DGENDB_PROFILE \
//       -I.../utils -o q19a q19a.cpp
//
// Run:   ./q19a <gendb_dir> <results_dir>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// -------------------------- raw mmap helpers --------------------------------
struct RawMap {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int fd = -1;

    RawMap() = default;
    RawMap(const std::string& p) { open(p); }
    void open(const std::string& path) {
        close();
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
        struct stat st{};
        fstat(fd, &st);
        size = (size_t)st.st_size;
        if (size > 0) {
            void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
            data = (const uint8_t*)p;
            madvise(const_cast<void*>((const void*)data), size, MADV_WILLNEED);
        }
    }
    void close() {
        if (data && size) munmap(const_cast<void*>((const void*)data), size);
        if (fd >= 0) ::close(fd);
        data = nullptr; size = 0; fd = -1;
    }
    ~RawMap() { close(); }
    RawMap(const RawMap&) = delete;
    RawMap& operator=(const RawMap&) = delete;
};

// ---------------------- varlen access ---------------------------------------
// off file: int64_t offsets, N+1 entries.
static inline const uint8_t* var_ptr(const int64_t* off, const uint8_t* dat, size_t row) {
    return dat + off[row];
}
static inline size_t var_len(const int64_t* off, size_t row) {
    return (size_t)(off[row + 1] - off[row]);
}

// ----------------------- lex compare ----------------------------------------
static inline int lex_cmp(const uint8_t* a, size_t la, const uint8_t* b, size_t lb) {
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

// ---------------------- CSV writer (one row) --------------------------------
static void csv_field(std::string& out, const uint8_t* p, size_t n) {
    bool need_quote = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        out.append((const char*)p, n);
        return;
    }
    out.push_back('"');
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '"') out.push_back('"');
        out.push_back((char)c);
    }
    out.push_back('"');
}

// ---------------------- dict lookup -----------------------------------------
// dict.off (int64_t offsets, K+1), dict.dat (bytes). Returns code (0..K-1) or -1.
static int dict_find(const int64_t* doff, size_t dcount, const uint8_t* ddat,
                     const char* needle, size_t nlen) {
    for (size_t i = 0; i < dcount; i++) {
        size_t s = (size_t)doff[i];
        size_t e = (size_t)doff[i + 1];
        if (e - s == nlen && std::memcmp(ddat + s, needle, nlen) == 0) return (int)i;
    }
    return -1;
}

// ---------------------- LIKE helpers ----------------------------------------
static inline bool contains(const uint8_t* hay, size_t hl, const char* nd, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    return memmem(hay, hl, nd, nl) != nullptr;
}

// mi.info LIKE 'Japan:%200%' OR LIKE 'USA:%200%'
static inline bool mi_info_match(const uint8_t* p, size_t n) {
    if (n >= 6 && std::memcmp(p, "Japan:", 6) == 0) {
        return memmem(p + 6, n - 6, "200", 3) != nullptr;
    }
    if (n >= 4 && std::memcmp(p, "USA:", 4) == 0) {
        return memmem(p + 4, n - 4, "200", 3) != nullptr;
    }
    return false;
}

// mc.note LIKE '%(USA)%' OR LIKE '%(worldwide)%'
static inline bool mc_note_match(const uint8_t* p, size_t n) {
    if (n == 0) return false;
    if (memmem(p, n, "(USA)", 5) != nullptr) return true;
    if (memmem(p, n, "(worldwide)", 11) != nullptr) return true;
    return false;
}

// ci.note IN 4 voice strings
static inline bool ci_note_voice(const uint8_t* p, size_t n) {
    switch (n) {
        case 7:  // "(voice)"
            return std::memcmp(p, "(voice)", 7) == 0;
        case 20: // "(voice) (uncredited)"
            return std::memcmp(p, "(voice) (uncredited)", 20) == 0;
        case 24: // "(voice: English version)"
            return std::memcmp(p, "(voice: English version)", 24) == 0;
        case 25: // "(voice: Japanese version)"
            return std::memcmp(p, "(voice: Japanese version)", 25) == 0;
        default:
            return false;
    }
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];

    // ------------------------- LOAD ----------------------------------------
    MmapColumn<int32_t> title_year, title_id;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> mc_movie_id, mc_company_id, mc_note_present_dummy; // mc_note via raw
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int32_t> ci_movie_id, ci_person_id, ci_role_id, ci_person_role_id;
    MmapColumn<int32_t> mc_off_idx, mi_off_idx, ci_off_idx, an_off_idx;

    RawMap title_title_off, title_title_dat;
    RawMap name_name_off, name_name_dat;
    RawMap role_off, role_dat;
    RawMap it_off, it_dat;
    RawMap cc_dict_off, cc_dict_dat;
    RawMap gender_dict_off, gender_dict_dat;
    RawMap mc_note_off, mc_note_dat;
    RawMap mi_info_off, mi_info_dat;
    RawMap ci_note_off, ci_note_dat;

    {
        GENDB_PHASE("data_loading");
        title_year.open(gdir + "/title/production_year.bin");
        title_id.open(gdir + "/title/id.bin");
        title_title_off.open(gdir + "/title/title.off");
        title_title_dat.open(gdir + "/title/title.dat");

        name_gender.open(gdir + "/name/gender.bin");
        name_name_off.open(gdir + "/name/name.off");
        name_name_dat.open(gdir + "/name/name.dat");

        cn_cc.open(gdir + "/company_name/country_code.bin");
        cc_dict_off.open(gdir + "/company_name/country_code.dict.off");
        cc_dict_dat.open(gdir + "/company_name/country_code.dict.dat");

        gender_dict_off.open(gdir + "/name/gender.dict.off");
        gender_dict_dat.open(gdir + "/name/gender.dict.dat");

        role_off.open(gdir + "/role_type/role.off");
        role_dat.open(gdir + "/role_type/role.dat");
        it_off.open(gdir + "/info_type/info.off");
        it_dat.open(gdir + "/info_type/info.dat");

        mc_movie_id.open(gdir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gdir + "/movie_companies/company_id.bin");
        mc_note_off.open(gdir + "/movie_companies/note.off");
        mc_note_dat.open(gdir + "/movie_companies/note.dat");

        mi_movie_id.open(gdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gdir + "/movie_info/info.off");
        mi_info_dat.open(gdir + "/movie_info/info.dat");

        ci_movie_id.open(gdir + "/cast_info/movie_id.bin");
        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_role_id.open(gdir + "/cast_info/role_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_note_off.open(gdir + "/cast_info/note.off");
        ci_note_dat.open(gdir + "/cast_info/note.dat");

        mc_off_idx.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off_idx.open(gdir + "/_idx/movie_info__movie_id__offsets.bin");
        ci_off_idx.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
        an_off_idx.open(gdir + "/_idx/aka_name__person_id__offsets.bin");
    }

    const int64_t* title_off_ptr = (const int64_t*)title_title_off.data;
    const uint8_t* title_dat_ptr = title_title_dat.data;
    const int64_t* name_off_ptr  = (const int64_t*)name_name_off.data;
    const uint8_t* name_dat_ptr  = name_name_dat.data;
    const int64_t* mc_note_off_ptr = (const int64_t*)mc_note_off.data;
    const uint8_t* mc_note_dat_ptr = mc_note_dat.data;
    const int64_t* mi_info_off_ptr = (const int64_t*)mi_info_off.data;
    const uint8_t* mi_info_dat_ptr = mi_info_dat.data;
    const int64_t* ci_note_off_ptr = (const int64_t*)ci_note_off.data;
    const uint8_t* ci_note_dat_ptr = ci_note_dat.data;

    const size_t N_title = title_year.count;
    const size_t N_name  = name_gender.count;

    // ----------------- resolve dict / scalar constants ---------------------
    int16_t cc_us;
    int8_t  g_f;
    int32_t rt_actress = -1;
    int32_t it_rd = -1;
    {
        GENDB_PHASE("resolve_dict_constants");
        {
            const int64_t* doff = (const int64_t*)cc_dict_off.data;
            size_t dcount = cc_dict_off.size / 8 - 1;
            int c = dict_find(doff, dcount, cc_dict_dat.data, "[us]", 4);
            if (c < 0) { std::fprintf(stderr, "cc_us not found\n"); return 1; }
            cc_us = (int16_t)c;
        }
        {
            const int64_t* doff = (const int64_t*)gender_dict_off.data;
            size_t dcount = gender_dict_off.size / 8 - 1;
            int c = dict_find(doff, dcount, gender_dict_dat.data, "f", 1);
            if (c < 0) { std::fprintf(stderr, "g_f not found\n"); return 1; }
            g_f = (int8_t)c;
        }
        {
            const int64_t* doff = (const int64_t*)role_off.data;
            size_t rcount = role_off.size / 8 - 1; // 12
            for (size_t i = 0; i < rcount; i++) {
                size_t s = (size_t)doff[i], e = (size_t)doff[i + 1];
                if (e - s == 7 && std::memcmp(role_dat.data + s, "actress", 7) == 0) {
                    rt_actress = (int32_t)(i + 1); // 1-based id
                    break;
                }
            }
            if (rt_actress < 0) { std::fprintf(stderr, "rt_actress not found\n"); return 1; }
        }
        {
            const int64_t* doff = (const int64_t*)it_off.data;
            size_t icount = it_off.size / 8 - 1; // 113
            const char* needle = "release dates";
            size_t nlen = 13;
            for (size_t i = 0; i < icount; i++) {
                size_t s = (size_t)doff[i], e = (size_t)doff[i + 1];
                if (e - s == nlen && std::memcmp(it_dat.data + s, needle, nlen) == 0) {
                    it_rd = (int32_t)(i + 1);
                    break;
                }
            }
            if (it_rd < 0) { std::fprintf(stderr, "it_rd not found\n"); return 1; }
        }
    }

    // ----------------- build P bitset over name.id domain -----------------
    // Use 1-based: bit at index pid (1..N_name). Vector size N_name+1.
    std::vector<uint64_t> P((N_name + 1 + 63) / 64, 0);
    {
        GENDB_PHASE("build_P_set");
        const int8_t* gptr = name_gender.data;
        #pragma omp parallel for schedule(static, 65536)
        for (size_t i = 0; i < N_name; i++) {
            if (gptr[i] != g_f) continue;
            size_t s = (size_t)name_off_ptr[i];
            size_t e = (size_t)name_off_ptr[i + 1];
            const uint8_t* p = name_dat_ptr + s;
            size_t n = e - s;
            if (n < 3) continue;
            if (memmem(p, n, "Ang", 3) == nullptr) continue;
            // pid = i + 1
            size_t pid = i + 1;
            uint64_t bit = (uint64_t)1 << (pid & 63);
            __atomic_fetch_or(&P[pid >> 6], bit, __ATOMIC_RELAXED);
        }
    }

    auto in_P = [&](int32_t pid) -> bool {
        if (pid <= 0 || (size_t)pid > N_name) return false;
        return (P[(size_t)pid >> 6] >> ((size_t)pid & 63)) & 1ULL;
    };

    // ----------------- main parallel scan ---------------------------------
    // For each thread: best name_id and best title_id (smallest lex).
    int nthreads = omp_get_max_threads();
    struct Best { int32_t name_id; int32_t title_id; };
    std::vector<Best> bests(nthreads, {-1, -1});

    auto title_str = [&](int32_t tid) {
        // tid is 1-based; row = tid - 1
        size_t r = (size_t)(tid - 1);
        size_t s = (size_t)title_off_ptr[r];
        size_t e = (size_t)title_off_ptr[r + 1];
        return std::pair<const uint8_t*, size_t>(title_dat_ptr + s, e - s);
    };
    auto name_str = [&](int32_t nid) {
        size_t r = (size_t)(nid - 1);
        size_t s = (size_t)name_off_ptr[r];
        size_t e = (size_t)name_off_ptr[r + 1];
        return std::pair<const uint8_t*, size_t>(name_dat_ptr + s, e - s);
    };

    {
        GENDB_PHASE("main_scan");
        const int32_t* mc_off = mc_off_idx.data;
        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* an_off = an_off_idx.data;
        const int32_t* yptr = title_year.data;
        const int32_t* mc_mid = mc_movie_id.data; // unused except for validation
        (void)mc_mid;
        const int32_t* mc_cid = mc_company_id.data;
        const int16_t* cc_ptr = cn_cc.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int32_t* ci_rid = ci_role_id.data;
        const int32_t* ci_prid = ci_person_role_id.data;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            int32_t local_name = -1;
            int32_t local_title = -1;
            // Cached strings for local best
            const uint8_t* local_name_p = nullptr; size_t local_name_n = 0;
            const uint8_t* local_title_p = nullptr; size_t local_title_n = 0;

            #pragma omp for schedule(static, 4096) nowait
            for (size_t r = 0; r < N_title; r++) {
                int32_t y = yptr[r];
                if (y < 2005 || y > 2009) continue;
                int32_t mv = (int32_t)(r + 1); // 1-based id

                // ---- mc range ----
                int32_t mcs = mc_off[mv];
                int32_t mce = mc_off[mv + 1];
                bool mc_ok = false;
                for (int32_t k = mcs; k < mce; k++) {
                    size_t s = (size_t)mc_note_off_ptr[k];
                    size_t e = (size_t)mc_note_off_ptr[k + 1];
                    if (e == s) continue; // NULL note
                    const uint8_t* p = mc_note_dat_ptr + s;
                    if (!mc_note_match(p, e - s)) continue;
                    int32_t comp = mc_cid[k];
                    if (comp <= 0) continue;
                    if (cc_ptr[comp - 1] != cc_us) continue;
                    mc_ok = true;
                    break;
                }
                if (!mc_ok) continue;

                // ---- mi range ----
                int32_t mis = mi_off[mv];
                int32_t mie = mi_off[mv + 1];
                bool mi_ok = false;
                for (int32_t k = mis; k < mie; k++) {
                    if (mi_itid[k] != it_rd) continue;
                    size_t s = (size_t)mi_info_off_ptr[k];
                    size_t e = (size_t)mi_info_off_ptr[k + 1];
                    if (e == s) continue;
                    if (!mi_info_match(mi_info_dat_ptr + s, e - s)) continue;
                    mi_ok = true;
                    break;
                }
                if (!mi_ok) continue;

                // ---- ci range ----
                int32_t cis = ci_off[mv];
                int32_t cie = ci_off[mv + 1];
                bool title_used = false;
                for (int32_t k = cis; k < cie; k++) {
                    if (ci_rid[k] != rt_actress) continue;
                    int32_t prid = ci_prid[k];
                    if (prid == INT32_MIN) continue;
                    int32_t pid = ci_pid[k];
                    if (!in_P(pid)) continue;
                    // ci.note in voice-set
                    size_t s = (size_t)ci_note_off_ptr[k];
                    size_t e = (size_t)ci_note_off_ptr[k + 1];
                    if (e == s) continue;
                    if (!ci_note_voice(ci_note_dat_ptr + s, e - s)) continue;
                    // aka_name existence
                    if (an_off[pid + 1] <= an_off[pid]) continue;

                    // Qualifying tuple — update MINs
                    if (!title_used) {
                        // candidate title for MIN
                        auto ts = title_str(mv);
                        if (local_title < 0 ||
                            lex_cmp(ts.first, ts.second, local_title_p, local_title_n) < 0) {
                            local_title = mv;
                            local_title_p = ts.first;
                            local_title_n = ts.second;
                        }
                        title_used = true;
                    }
                    auto ns = name_str(pid);
                    if (local_name < 0 ||
                        lex_cmp(ns.first, ns.second, local_name_p, local_name_n) < 0) {
                        local_name = pid;
                        local_name_p = ns.first;
                        local_name_n = ns.second;
                    }
                }
            } // end for r

            bests[tid] = {local_name, local_title};
        } // end parallel
    } // end main_scan

    // ---- reduce ----
    int32_t best_name = -1;
    int32_t best_title = -1;
    const uint8_t* bn_p = nullptr; size_t bn_n = 0;
    const uint8_t* bt_p = nullptr; size_t bt_n = 0;
    for (auto& b : bests) {
        if (b.name_id > 0) {
            auto ns = name_str(b.name_id);
            if (best_name < 0 || lex_cmp(ns.first, ns.second, bn_p, bn_n) < 0) {
                best_name = b.name_id;
                bn_p = ns.first; bn_n = ns.second;
            }
        }
        if (b.title_id > 0) {
            auto ts = title_str(b.title_id);
            if (best_title < 0 || lex_cmp(ts.first, ts.second, bt_p, bt_n) < 0) {
                best_title = b.title_id;
                bt_p = ts.first; bt_n = ts.second;
            }
        }
    }

    // ---------------------- output ----------------------------------------
    {
        GENDB_PHASE("output");
        // Ensure results_dir exists
        mkdir(rdir.c_str(), 0755);
        std::string path = rdir + "/Q19a.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }

        std::string line;
        line = "voicing_actress,voiced_movie\n";
        std::fwrite(line.data(), 1, line.size(), f);

        line.clear();
        if (best_name > 0) csv_field(line, bn_p, bn_n);
        line.push_back(',');
        if (best_title > 0) csv_field(line, bt_p, bt_n);
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }

    return 0;
}
