// Q24b — MIN(chn.name), MIN(n.name), MIN(t.title)
//
// Build:
//   g++ -O3 -march=native -std=c++17 -Wall -lpthread -fopenmp -DGENDB_PROFILE \
//       -I.../utils -o q24b q24b.cpp
//
// Run:   ./q24b <gendb_dir> <results_dir>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
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
    explicit RawMap(const std::string& p) { open(p); }
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

// mi.info LIKE 'Japan:%201%' OR LIKE 'USA:%201%'
static inline bool mi_info_match(const uint8_t* p, size_t n) {
    if (n >= 6 && std::memcmp(p, "Japan:", 6) == 0) {
        return memmem(p + 6, n - 6, "201", 3) != nullptr;
    }
    if (n >= 4 && std::memcmp(p, "USA:", 4) == 0) {
        return memmem(p + 4, n - 4, "201", 3) != nullptr;
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
    MmapColumn<uint8_t> name_gender;
    MmapColumn<uint16_t> cn_cc;
    MmapColumn<int32_t> mc_movie_id, mc_company_id;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int32_t> mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t> ci_movie_id, ci_person_id, ci_role_id, ci_person_role_id;
    MmapColumn<int32_t> mc_off_idx, mi_off_idx, mk_off_idx, ci_off_idx, an_off_idx;

    RawMap title_title_off, title_title_dat;
    RawMap name_name_off, name_name_dat;
    RawMap chn_name_off, chn_name_dat;
    RawMap role_off, role_dat;
    RawMap it_off, it_dat;
    RawMap kw_off, kw_dat;
    RawMap cn_name_off, cn_name_dat;
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

        chn_name_off.open(gdir + "/char_name/name.off");
        chn_name_dat.open(gdir + "/char_name/name.dat");

        cn_cc.open(gdir + "/company_name/country_code.bin");
        cn_name_off.open(gdir + "/company_name/name.off");
        cn_name_dat.open(gdir + "/company_name/name.dat");
        cc_dict_off.open(gdir + "/company_name/country_code.dict.off");
        cc_dict_dat.open(gdir + "/company_name/country_code.dict.dat");

        gender_dict_off.open(gdir + "/name/gender.dict.off");
        gender_dict_dat.open(gdir + "/name/gender.dict.dat");

        role_off.open(gdir + "/role_type/role.off");
        role_dat.open(gdir + "/role_type/role.dat");
        it_off.open(gdir + "/info_type/info.off");
        it_dat.open(gdir + "/info_type/info.dat");
        kw_off.open(gdir + "/keyword/keyword.off");
        kw_dat.open(gdir + "/keyword/keyword.dat");

        mc_movie_id.open(gdir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gdir + "/movie_companies/company_id.bin");

        mi_movie_id.open(gdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gdir + "/movie_info/info.off");
        mi_info_dat.open(gdir + "/movie_info/info.dat");

        mk_movie_id.open(gdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");

        ci_movie_id.open(gdir + "/cast_info/movie_id.bin");
        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_role_id.open(gdir + "/cast_info/role_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_note_off.open(gdir + "/cast_info/note.off");
        ci_note_dat.open(gdir + "/cast_info/note.dat");

        mc_off_idx.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off_idx.open(gdir + "/_idx/movie_info__movie_id__offsets.bin");
        mk_off_idx.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        ci_off_idx.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
        an_off_idx.open(gdir + "/_idx/aka_name__person_id__offsets.bin");
    }

    const int64_t* title_off_ptr = (const int64_t*)title_title_off.data;
    const uint8_t* title_dat_ptr = title_title_dat.data;
    const int64_t* name_off_ptr  = (const int64_t*)name_name_off.data;
    const uint8_t* name_dat_ptr  = name_name_dat.data;
    const int64_t* chn_off_ptr   = (const int64_t*)chn_name_off.data;
    const uint8_t* chn_dat_ptr   = chn_name_dat.data;
    const int64_t* mi_info_off_ptr = (const int64_t*)mi_info_off.data;
    const uint8_t* mi_info_dat_ptr = mi_info_dat.data;
    const int64_t* ci_note_off_ptr = (const int64_t*)ci_note_off.data;
    const uint8_t* ci_note_dat_ptr = ci_note_dat.data;

    const size_t N_title = title_year.count;
    const size_t N_name  = name_gender.count;

    // ----------------- resolve dict / scalar constants ---------------------
    uint16_t cc_us = 0;
    uint8_t  g_f = 0;
    int32_t rt_actress = -1;
    int32_t it_rd = -1;
    int32_t target_cn_id = -1;
    std::array<int32_t, 4> kw_ids = { -1, -1, -1, -1 };
    int num_kw = 0;

    {
        GENDB_PHASE("resolve_dict_constants");
        {
            const int64_t* doff = (const int64_t*)cc_dict_off.data;
            size_t dcount = cc_dict_off.size / 8 - 1;
            int c = dict_find(doff, dcount, cc_dict_dat.data, "[us]", 4);
            if (c < 0) { std::fprintf(stderr, "cc_us not found\n"); return 1; }
            // Encoded values are 1-based (0 reserved for NULL).
            cc_us = (uint16_t)(c + 1);
        }
        {
            const int64_t* doff = (const int64_t*)gender_dict_off.data;
            size_t dcount = gender_dict_off.size / 8 - 1;
            int c = dict_find(doff, dcount, gender_dict_dat.data, "f", 1);
            if (c < 0) { std::fprintf(stderr, "g_f not found\n"); return 1; }
            g_f = (uint8_t)(c + 1);
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
        {
            // keywords: hero, martial-arts, hand-to-hand-combat, computer-animated-movie
            static const char* kws[4] = {
                "hero", "martial-arts", "hand-to-hand-combat", "computer-animated-movie"
            };
            static const size_t klen[4] = { 4, 12, 19, 23 };
            const int64_t* doff = (const int64_t*)kw_off.data;
            size_t kcount = kw_off.size / 8 - 1;
            for (size_t i = 0; i < kcount && num_kw < 4; i++) {
                size_t s = (size_t)doff[i], e = (size_t)doff[i + 1];
                size_t len = e - s;
                for (int j = 0; j < 4; j++) {
                    if (len == klen[j] && std::memcmp(kw_dat.data + s, kws[j], len) == 0) {
                        kw_ids[num_kw++] = (int32_t)(i + 1);
                        break;
                    }
                }
            }
        }
        {
            // target_cn_id: scan company_name.name for 'DreamWorks Animation'
            const int64_t* doff = (const int64_t*)cn_name_off.data;
            size_t ccount = cn_name_off.size / 8 - 1;
            const char* needle = "DreamWorks Animation";
            size_t nlen = 20;
            for (size_t i = 0; i < ccount; i++) {
                size_t s = (size_t)doff[i], e = (size_t)doff[i + 1];
                if (e - s == nlen && std::memcmp(cn_name_dat.data + s, needle, nlen) == 0) {
                    target_cn_id = (int32_t)(i + 1);
                    break;
                }
            }
            if (target_cn_id < 0) {
                std::fprintf(stderr, "target_cn_id not found\n"); return 1;
            }
            // verify country_code == us_code
            if (cn_cc.data[target_cn_id - 1] != cc_us) {
                std::fprintf(stderr, "warning: target_cn country_code != us_code\n");
            }
        }
    }

    // ----------------- build valid_persons bitset --------------------------
    std::vector<uint64_t> P((N_name + 1 + 63) / 64, 0);
    {
        GENDB_PHASE("build_valid_persons");
        const uint8_t* gptr = name_gender.data;
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
    auto chn_str = [&](int32_t cid) {
        size_t r = (size_t)(cid - 1);
        size_t s = (size_t)chn_off_ptr[r];
        size_t e = (size_t)chn_off_ptr[r + 1];
        return std::pair<const uint8_t*, size_t>(chn_dat_ptr + s, e - s);
    };

    // ---- running mins ----
    int32_t best_chn_id = -1;
    int32_t best_name_id = -1;
    int32_t best_title_id = -1;
    const uint8_t* bc_p = nullptr; size_t bc_n = 0;
    const uint8_t* bn_p = nullptr; size_t bn_n = 0;
    const uint8_t* bt_p = nullptr; size_t bt_n = 0;

    {
        GENDB_PHASE("main_scan");
        const int32_t* mc_off = mc_off_idx.data;
        const int32_t* mi_off = mi_off_idx.data;
        const int32_t* mk_off = mk_off_idx.data;
        const int32_t* ci_off = ci_off_idx.data;
        const int32_t* an_off = an_off_idx.data;
        const int32_t* yptr = title_year.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int32_t* mi_itid = mi_info_type_id.data;
        const int32_t* mk_kid = mk_keyword_id.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int32_t* ci_rid = ci_role_id.data;
        const int32_t* ci_prid = ci_person_role_id.data;

        for (size_t r = 0; r < N_title; r++) {
            // ---- title prefix filter FIRST (very selective) ----
            size_t ts = (size_t)title_off_ptr[r];
            size_t te = (size_t)title_off_ptr[r + 1];
            size_t tn = te - ts;
            if (tn < 13) continue;
            if (std::memcmp(title_dat_ptr + ts, "Kung Fu Panda", 13) != 0) continue;

            // ---- year > 2010 ----
            int32_t y = yptr[r];
            if (y <= 2010) continue;

            int32_t mv = (int32_t)(r + 1);

            // ---- mc range: company_id == target_cn_id ----
            int32_t mcs = mc_off[mv];
            int32_t mce = mc_off[mv + 1];
            bool mc_ok = false;
            for (int32_t k = mcs; k < mce; k++) {
                if (mc_cid[k] == target_cn_id) { mc_ok = true; break; }
            }
            if (!mc_ok) continue;

            // ---- mk range: keyword_id in kw_ids ----
            int32_t mks = mk_off[mv];
            int32_t mke = mk_off[mv + 1];
            bool mk_ok = false;
            for (int32_t k = mks; k < mke; k++) {
                int32_t kid = mk_kid[k];
                for (int j = 0; j < num_kw; j++) {
                    if (kid == kw_ids[j]) { mk_ok = true; break; }
                }
                if (mk_ok) break;
            }
            if (!mk_ok) continue;

            // ---- mi range: info_type_id==it_rd AND LIKE ----
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

            // ---- ci range: produces (person_id, person_role_id) tuples ----
            int32_t cis = ci_off[mv];
            int32_t cie = ci_off[mv + 1];
            bool any_tuple = false;
            for (int32_t k = cis; k < cie; k++) {
                if (ci_rid[k] != rt_actress) continue;
                int32_t prid = ci_prid[k];
                if (prid == INT32_MIN || prid <= 0) continue;
                int32_t pid = ci_pid[k];
                if (!in_P(pid)) continue;
                // ci.note in voice-set
                size_t s = (size_t)ci_note_off_ptr[k];
                size_t e = (size_t)ci_note_off_ptr[k + 1];
                if (e == s) continue;
                if (!ci_note_voice(ci_note_dat_ptr + s, e - s)) continue;
                // aka_name existence
                if (an_off[pid + 1] <= an_off[pid]) continue;

                // --- qualifying tuple ---
                any_tuple = true;
                // MIN(t.title)
                {
                    auto str = std::pair<const uint8_t*, size_t>(title_dat_ptr + ts, tn);
                    if (best_title_id < 0 ||
                        lex_cmp(str.first, str.second, bt_p, bt_n) < 0) {
                        best_title_id = mv;
                        bt_p = str.first; bt_n = str.second;
                    }
                }
                // MIN(n.name)
                {
                    auto ns = name_str(pid);
                    if (best_name_id < 0 ||
                        lex_cmp(ns.first, ns.second, bn_p, bn_n) < 0) {
                        best_name_id = pid;
                        bn_p = ns.first; bn_n = ns.second;
                    }
                }
                // MIN(chn.name)
                {
                    auto cs = chn_str(prid);
                    if (best_chn_id < 0 ||
                        lex_cmp(cs.first, cs.second, bc_p, bc_n) < 0) {
                        best_chn_id = prid;
                        bc_p = cs.first; bc_n = cs.second;
                    }
                }
            }
            (void)any_tuple;
        }
    }

    // ---------------------- output ----------------------------------------
    {
        GENDB_PHASE("output");
        mkdir(rdir.c_str(), 0755);
        std::string path = rdir + "/Q24b.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }

        std::string line;
        line = "voiced_char_name,voicing_actress_name,kung_fu_panda\n";
        std::fwrite(line.data(), 1, line.size(), f);

        line.clear();
        if (best_chn_id > 0) csv_field(line, bc_p, bc_n);
        line.push_back(',');
        if (best_name_id > 0) csv_field(line, bn_p, bn_n);
        line.push_back(',');
        if (best_title_id > 0) csv_field(line, bt_p, bt_n);
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }

    return 0;
}
