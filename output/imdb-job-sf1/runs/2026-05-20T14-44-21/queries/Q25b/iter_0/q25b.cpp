// Q25b - GenDB iter_0
// SELECT MIN(mi.info), MIN(mi_idx.info), MIN(n.name), MIN(t.title)
// Filters: ci.note IN writer-set, it1.info='genres', it2.info='votes',
//          k.keyword IN {murder,blood,gore,death,female-nudity},
//          mi.info='Horror', n.gender='m',
//          t.production_year > 2010 AND t.title LIKE 'Vampire%'

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
struct VarlenCol {
    MmapColumn<uint64_t> off;
    MmapColumn<char>     dat;
    size_t rows() const { return off.count - 1; }
    std::string_view get(size_t i) const {
        uint64_t lo = off.data[i], hi = off.data[i + 1];
        return std::string_view(dat.data + lo, hi - lo);
    }
};

static inline void open_varlen(VarlenCol& v, const std::string& base) {
    v.off.open(base + ".off");
    v.dat.open(base + ".dat");
}

static inline bool csv_needs_quote(std::string_view s) {
    for (char c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
    return false;
}
static inline void csv_write(FILE* f, std::string_view s) {
    if (s.empty()) return;
    if (!csv_needs_quote(s)) {
        std::fwrite(s.data(), 1, s.size(), f);
    } else {
        std::fputc('"', f);
        for (char c : s) {
            if (c == '"') std::fputc('"', f);
            std::fputc(c, f);
        }
        std::fputc('"', f);
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdb = argv[1];
    std::string out = argv[2];
    mkdir(out.c_str(), 0755);

    GENDB_PHASE("total");

    // ----- Dimension resolution -----
    int32_t it1_id = -1, it2_id = -1;
    std::vector<int32_t> kw_ids;
    uint8_t m_code = 0;
    std::vector<uint64_t> valid_persons; // bitset, indexed by (id-1)
    size_t name_rows = 0;

    // movie_keyword needs name_rows; defer building bitset

    {
        GENDB_PHASE("data_loading");

        // ----- info_type -----
        {
            MmapColumn<int32_t> id_col(gdb + "/info_type/id.bin");
            VarlenCol info; open_varlen(info, gdb + "/info_type/info");
            size_t n = id_col.count;
            for (size_t i = 0; i < n; ++i) {
                auto s = info.get(i);
                if (s.size() == 6 && std::memcmp(s.data(), "genres", 6) == 0) it1_id = id_col.data[i];
                else if (s.size() == 5 && std::memcmp(s.data(), "votes", 5) == 0) it2_id = id_col.data[i];
            }
        }

        // ----- keyword -----
        {
            MmapColumn<int32_t> id_col(gdb + "/keyword/id.bin");
            VarlenCol kw; open_varlen(kw, gdb + "/keyword/keyword");
            static const char* wanted[5] = {"murder", "blood", "gore", "death", "female-nudity"};
            static const size_t wlen[5] = {6, 5, 4, 5, 13};
            size_t n = id_col.count;
            for (size_t i = 0; i < n; ++i) {
                auto s = kw.get(i);
                for (int k = 0; k < 5; ++k) {
                    if (s.size() == wlen[k] && std::memcmp(s.data(), wanted[k], wlen[k]) == 0) {
                        kw_ids.push_back(id_col.data[i]);
                        break;
                    }
                }
            }
            std::sort(kw_ids.begin(), kw_ids.end());
        }

        // ----- gender m_code -----
        {
            MmapColumn<uint64_t> doff(gdb + "/name/gender.dict.off");
            MmapColumn<char>     ddat(gdb + "/name/gender.dict.dat");
            size_t n = doff.count - 1;
            // Dict convention: code 0 = NULL, codes start at 1 (i.e., code = slot_index + 1)
            for (size_t i = 0; i < n; ++i) {
                uint64_t lo = doff.data[i], hi = doff.data[i+1];
                if (hi - lo == 1 && ddat.data[lo] == 'm') { m_code = (uint8_t)(i + 1); break; }
            }
        }

        // ----- valid_persons bitset (name.gender == m_code) -----
        {
            MmapColumn<uint8_t> g(gdb + "/name/gender.bin");
            name_rows = g.count;
            size_t nw = (name_rows + 63) / 64;
            valid_persons.assign(nw, 0ULL);
            const uint8_t* gp = g.data;
            const uint8_t mc = m_code;
            for (size_t i = 0; i < name_rows; ++i) {
                if (gp[i] == mc) valid_persons[i >> 6] |= (1ULL << (i & 63));
            }
        }
    }

    // ----- Map all required columns -----
    // title
    VarlenCol t_title;       open_varlen(t_title,    gdb + "/title/title");
    MmapColumn<int32_t> t_year(gdb + "/title/production_year.bin");
    size_t title_rows = t_year.count;

    // movie_keyword
    MmapColumn<int32_t> mk_off(gdb + "/_idx/movie_keyword__movie_id__offsets.bin");
    MmapColumn<int32_t> mk_kwid(gdb + "/movie_keyword/keyword_id.bin");

    // movie_info_idx
    MmapColumn<int32_t> mii_off(gdb + "/_idx/movie_info_idx__movie_id__offsets.bin");
    MmapColumn<int32_t> mii_itid(gdb + "/movie_info_idx/info_type_id.bin");
    VarlenCol mii_info; open_varlen(mii_info, gdb + "/movie_info_idx/info");

    // movie_info
    MmapColumn<int32_t> mi_off(gdb + "/_idx/movie_info__movie_id__offsets.bin");
    MmapColumn<int32_t> mi_itid(gdb + "/movie_info/info_type_id.bin");
    VarlenCol mi_info; open_varlen(mi_info, gdb + "/movie_info/info");

    // cast_info
    MmapColumn<int32_t> ci_off(gdb + "/_idx/cast_info__movie_id__offsets.bin");
    MmapColumn<int32_t> ci_pid(gdb + "/cast_info/person_id.bin");
    VarlenCol ci_note; open_varlen(ci_note, gdb + "/cast_info/note");

    // name (for projection)
    VarlenCol n_name; open_varlen(n_name, gdb + "/name/name");

    // Hints
    mk_off.advise_random();   mk_kwid.advise_random();
    mii_off.advise_random();  mii_itid.advise_random();
    mi_off.advise_random();   mi_itid.advise_random();
    ci_off.advise_random();   ci_pid.advise_random();

    // ----- Driver: title filtered (year > 2010 AND title LIKE 'Vampire%') -----
    // Project: MIN aggregations over 4 string columns
    std::string min_mi_info, min_mii_info, min_n_name, min_t_title;
    bool have = false;

    auto update_min = [](std::string& cur, std::string_view s, bool& seen) {
        if (s.empty()) return;
        if (!seen) { cur.assign(s.data(), s.size()); seen = true; }
        else if (std::string_view(cur) > s) { cur.assign(s.data(), s.size()); }
    };
    bool seen_mi = false, seen_mii = false, seen_n = false, seen_t = false;

    static const char PREFIX[] = "Vampire";
    static constexpr size_t PLEN = 7;

    // Writer-note set (5 entries)
    static const char* WNOTE[5] = {"(writer)", "(head writer)", "(written by)", "(story)", "(story editor)"};
    static const size_t WLEN[5] = {8, 13, 12, 7, 14};

    auto note_in_writer = [](std::string_view s) -> bool {
        for (int k = 0; k < 5; ++k) {
            if (s.size() == WLEN[k] && std::memcmp(s.data(), WNOTE[k], WLEN[k]) == 0) return true;
        }
        return false;
    };

    auto kw_match = [&](int32_t kid) -> bool {
        for (int32_t k : kw_ids) if (k == kid) return true;
        return false;
    };

    {
        GENDB_PHASE("main_scan");

        for (size_t i = 0; i < title_rows; ++i) {
            int32_t y = t_year.data[i];
            if (y <= 2010) continue;
            // title prefix check
            uint64_t lo = t_title.off.data[i], hi = t_title.off.data[i+1];
            if (hi - lo < PLEN) continue;
            if (std::memcmp(t_title.dat.data + lo, PREFIX, PLEN) != 0) continue;

            int32_t v = (int32_t)(i + 1); // title.id (dense from 1)

            // ----- Probe movie_keyword -----
            int32_t a = mk_off.data[v], b = mk_off.data[v + 1];
            bool mk_ok = false;
            for (int32_t r = a; r < b; ++r) {
                if (kw_match(mk_kwid.data[r])) { mk_ok = true; break; }
            }
            if (!mk_ok) continue;

            // ----- Probe movie_info_idx (filter info_type_id == it2_id), collect info -----
            a = mii_off.data[v]; b = mii_off.data[v + 1];
            bool mii_ok = false;
            std::string_view local_mii_min{};
            for (int32_t r = a; r < b; ++r) {
                if (mii_itid.data[r] == it2_id) {
                    auto s = mii_info.get((size_t)r);
                    if (s.empty()) continue;
                    if (!mii_ok || s < local_mii_min) local_mii_min = s;
                    mii_ok = true;
                }
            }
            if (!mii_ok) continue;

            // ----- Probe movie_info (info_type_id == it1_id AND info == "Horror") -----
            a = mi_off.data[v]; b = mi_off.data[v + 1];
            bool mi_ok = false;
            std::string_view local_mi_min{};
            for (int32_t r = a; r < b; ++r) {
                if (mi_itid.data[r] != it1_id) continue;
                auto s = mi_info.get((size_t)r);
                if (s.size() != 6 || std::memcmp(s.data(), "Horror", 6) != 0) continue;
                if (!mi_ok || s < local_mi_min) local_mi_min = s;
                mi_ok = true;
            }
            if (!mi_ok) continue;

            // ----- Probe cast_info (note IN writer-set AND valid_persons[person_id-1]) -----
            a = ci_off.data[v]; b = ci_off.data[v + 1];
            bool ci_ok = false;
            std::string_view local_n_min{};
            // need scratch buffer for name since string_view is reused across iterations
            std::string local_n_min_owned;
            for (int32_t r = a; r < b; ++r) {
                auto note = ci_note.get((size_t)r);
                if (!note_in_writer(note)) continue;
                int32_t pid = ci_pid.data[r];
                if (pid <= 0 || (size_t)pid > name_rows) continue;
                size_t bi = (size_t)(pid - 1);
                if (!((valid_persons[bi >> 6] >> (bi & 63)) & 1ULL)) continue;
                auto nm = n_name.get(bi);
                if (nm.empty()) continue;
                if (!ci_ok || nm < std::string_view(local_n_min_owned)) {
                    local_n_min_owned.assign(nm.data(), nm.size());
                }
                ci_ok = true;
            }
            if (!ci_ok) continue;

            // ----- This title contributes -----
            have = true;
            update_min(min_mi_info,  local_mi_min,  seen_mi);
            update_min(min_mii_info, local_mii_min, seen_mii);
            // For name we kept owned copy
            if (!seen_n) { min_n_name = local_n_min_owned; seen_n = true; }
            else if (std::string_view(min_n_name) > std::string_view(local_n_min_owned)) {
                min_n_name = local_n_min_owned;
            }
            // t.title
            std::string_view ts(t_title.dat.data + lo, hi - lo);
            update_min(min_t_title, ts, seen_t);
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::string outpath = out + "/Q25b.csv";
        FILE* f = std::fopen(outpath.c_str(), "w");
        if (!f) { std::perror("fopen"); return 1; }
        std::fputs("movie_budget,movie_votes,male_writer,violent_movie_title\n", f);
        if (have) {
            csv_write(f, min_mi_info);  std::fputc(',', f);
            csv_write(f, min_mii_info); std::fputc(',', f);
            csv_write(f, min_n_name);   std::fputc(',', f);
            csv_write(f, min_t_title);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
