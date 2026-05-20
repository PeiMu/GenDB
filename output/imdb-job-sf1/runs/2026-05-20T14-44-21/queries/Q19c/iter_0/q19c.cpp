// Q19c — MIN(n.name), MIN(t.title)
// Plan: drive from movie_info via info_type_id CSR (it_rd slice).
//   For each mi row in the release-dates slice, filter on
//     mi.info LIKE 'Japan:%200%' OR 'USA:%200%'
//   collect movie_id, dedup, then probe mc (cn.country_code='[us]'),
//   probe ci (note IN voice-set, role=actress, person_role_id present),
//   require n.gender='f' AND n.name LIKE '%An%', require aka_name range
//   non-empty for person.
//
// Build:
//   g++ -O3 -march=native -std=c++17 -Wall -lpthread -fopenmp -DGENDB_PROFILE \
//       -I.../utils -o q19c q19c.cpp
//
// Run: ./q19c <gendb_dir> <results_dir>

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
    MmapColumn<int32_t> title_year;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> mc_movie_id, mc_company_id;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int32_t> ci_person_id, ci_role_id, ci_person_role_id;
    MmapColumn<int32_t> mc_off_idx, ci_off_idx, an_off_idx;
    MmapColumn<int32_t> mit_off_idx, mit_row_idx;

    RawMap title_title_off, title_title_dat;
    RawMap name_name_off, name_name_dat;
    RawMap role_off, role_dat;
    RawMap it_off, it_dat;
    RawMap cc_dict_off, cc_dict_dat;
    RawMap gender_dict_off, gender_dict_dat;
    RawMap mi_info_off, mi_info_dat;
    RawMap ci_note_off, ci_note_dat;

    {
        GENDB_PHASE("data_loading");
        title_year.open(gdir + "/title/production_year.bin");
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

        mi_movie_id.open(gdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gdir + "/movie_info/info.off");
        mi_info_dat.open(gdir + "/movie_info/info.dat");

        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_role_id.open(gdir + "/cast_info/role_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_note_off.open(gdir + "/cast_info/note.off");
        ci_note_dat.open(gdir + "/cast_info/note.dat");

        mc_off_idx.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");
        ci_off_idx.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
        an_off_idx.open(gdir + "/_idx/aka_name__person_id__offsets.bin");

        mit_off_idx.open(gdir + "/_idx/movie_info__info_type_id__offsets.bin");
        mit_row_idx.open(gdir + "/_idx/movie_info__info_type_id__rowids.bin");
    }

    const int64_t* title_off_ptr = (const int64_t*)title_title_off.data;
    const uint8_t* title_dat_ptr = title_title_dat.data;
    const int64_t* name_off_ptr  = (const int64_t*)name_name_off.data;
    const uint8_t* name_dat_ptr  = name_name_dat.data;
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
            // Stored encoding in country_code.bin is (dict_position + 1); 0 = NULL.
            cc_us = (int16_t)(c + 1);
        }
        {
            const int64_t* doff = (const int64_t*)gender_dict_off.data;
            size_t dcount = gender_dict_off.size / 8 - 1;
            int c = dict_find(doff, dcount, gender_dict_dat.data, "f", 1);
            if (c < 0) { std::fprintf(stderr, "g_f not found\n"); return 1; }
            // Stored encoding in gender.bin is (dict_position + 1); 0 = NULL.
            g_f = (int8_t)(c + 1);
        }
        {
            const int64_t* doff = (const int64_t*)role_off.data;
            size_t rcount = role_off.size / 8 - 1;
            for (size_t i = 0; i < rcount; i++) {
                size_t s = (size_t)doff[i], e = (size_t)doff[i + 1];
                if (e - s == 7 && std::memcmp(role_dat.data + s, "actress", 7) == 0) {
                    rt_actress = (int32_t)(i + 1);
                    break;
                }
            }
            if (rt_actress < 0) { std::fprintf(stderr, "rt_actress not found\n"); return 1; }
        }
        {
            const int64_t* doff = (const int64_t*)it_off.data;
            size_t icount = it_off.size / 8 - 1;
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
    // P[pid] = 1 iff name[pid-1].gender=='f' AND name[pid-1] contains "An".
    std::vector<uint64_t> P((N_name + 1 + 63) / 64, 0);
    {
        GENDB_PHASE("build_P_set");
        const int8_t* gptr = name_gender.data;
        #pragma omp parallel for schedule(static, 65536)
        for (size_t i = 0; i < N_name; i++) {
            if (gptr[i] != g_f) continue;
            size_t s = (size_t)name_off_ptr[i];
            size_t e = (size_t)name_off_ptr[i + 1];
            size_t n = e - s;
            if (n < 2) continue;
            const uint8_t* p = name_dat_ptr + s;
            if (memmem(p, n, "An", 2) == nullptr) continue;
            size_t pid = i + 1;
            uint64_t bit = (uint64_t)1 << (pid & 63);
            __atomic_fetch_or(&P[pid >> 6], bit, __ATOMIC_RELAXED);
        }
    }

    auto in_P = [&](int32_t pid) -> bool {
        if (pid <= 0 || (size_t)pid > N_name) return false;
        return (P[(size_t)pid >> 6] >> ((size_t)pid & 63)) & 1ULL;
    };

    // ----------------- Phase 1: scan mi CSR slice for it_rd -----------------
    // For each mi row in the release_dates slice, filter on LIKE patterns
    // and t.production_year > 2000; collect surviving movie_id into T.
    std::vector<int32_t> T;
    {
        GENDB_PHASE("scan_mi_csr_and_filter");
        const int32_t* mit_off = mit_off_idx.data;
        const int32_t* mit_row = mit_row_idx.data;
        const int32_t* mi_mid  = mi_movie_id.data;
        const int32_t* yptr    = title_year.data;

        int32_t lo = mit_off[it_rd];
        int32_t hi = mit_off[it_rd + 1];

        int nthreads = omp_get_max_threads();
        std::vector<std::vector<int32_t>> local_T(nthreads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::vector<int32_t>& lt = local_T[tid];
            lt.reserve(1024);
            #pragma omp for schedule(static, 4096) nowait
            for (int32_t k = lo; k < hi; k++) {
                int32_t r = mit_row[k];
                size_t s = (size_t)mi_info_off_ptr[r];
                size_t e = (size_t)mi_info_off_ptr[r + 1];
                if (e == s) continue;
                if (!mi_info_match(mi_info_dat_ptr + s, e - s)) continue;
                int32_t mv = mi_mid[r];
                if (mv <= 0 || (size_t)mv > N_title) continue;
                int32_t y = yptr[mv - 1];
                if (y == INT32_MIN) continue;
                if (y <= 2000) continue;
                lt.push_back(mv);
            }
        }
        // Merge
        size_t total = 0;
        for (auto& v : local_T) total += v.size();
        T.reserve(total);
        for (auto& v : local_T) T.insert(T.end(), v.begin(), v.end());
        // Dedup (a movie can have many release-date rows matching the LIKE)
        std::sort(T.begin(), T.end());
        T.erase(std::unique(T.begin(), T.end()), T.end());
    }

    // ----------------- Phase 2: probe mc, ci, aggregate -------------------
    int nthreads = omp_get_max_threads();
    struct Best { int32_t name_id; int32_t title_id; };
    std::vector<Best> bests(nthreads, {-1, -1});

    auto title_str = [&](int32_t tid) {
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
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* an_off = an_off_idx.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int16_t* cc_ptr = cn_cc.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int32_t* ci_rid = ci_role_id.data;
        const int32_t* ci_prid = ci_person_role_id.data;

        size_t Nt = T.size();
        const int32_t* Tdata = T.data();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            int32_t local_name = -1;
            int32_t local_title = -1;
            const uint8_t* local_name_p = nullptr; size_t local_name_n = 0;
            const uint8_t* local_title_p = nullptr; size_t local_title_n = 0;

            #pragma omp for schedule(static, 64) nowait
            for (size_t i = 0; i < Nt; i++) {
                int32_t mv = Tdata[i];

                // ---- mc range: require any cn.country_code=='[us]' ----
                int32_t mcs = mc_off[mv];
                int32_t mce = mc_off[mv + 1];
                bool mc_ok = false;
                for (int32_t k = mcs; k < mce; k++) {
                    int32_t comp = mc_cid[k];
                    if (comp <= 0) continue;
                    if (cc_ptr[comp - 1] != cc_us) continue;
                    mc_ok = true;
                    break;
                }
                if (!mc_ok) continue;

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
                    size_t s = (size_t)ci_note_off_ptr[k];
                    size_t e = (size_t)ci_note_off_ptr[k + 1];
                    if (e == s) continue;
                    if (!ci_note_voice(ci_note_dat_ptr + s, e - s)) continue;
                    // aka_name existence
                    if (an_off[pid + 1] <= an_off[pid]) continue;

                    if (!title_used) {
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
            }

            bests[tid] = {local_name, local_title};
        }
    }

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
        mkdir(rdir.c_str(), 0755);
        std::string path = rdir + "/Q19c.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }

        std::string line;
        line = "voicing_actress,jap_engl_voiced_movie\n";
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
