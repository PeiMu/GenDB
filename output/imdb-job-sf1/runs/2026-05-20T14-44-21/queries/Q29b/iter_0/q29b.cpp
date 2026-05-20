// Q29b — Shrek 2 voice cast cross-join
// Driver: title varlen scan for 'Shrek 2', tiny match set → per-movie probes.
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <sys/stat.h>
#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---------- dim id resolution ----------
// Find varlen row whose contents equal literal; returns -1 if not found.
static int32_t find_varlen_eq(const uint64_t* off, const char* dat, size_t n,
                              const char* lit, size_t lit_len) {
    for (size_t i = 0; i < n; i++) {
        size_t s = off[i], e = off[i + 1];
        if ((e - s) == lit_len && std::memcmp(dat + s, lit, lit_len) == 0) return (int32_t)i;
    }
    return -1;
}

// LIKE 'USA:%200%' — starts with "USA:" and somewhere later contains "200"
static inline bool like_usa_200(const char* s, size_t len) {
    if (len < 7) return false; // USA: + 200
    if (std::memcmp(s, "USA:", 4) != 0) return false;
    // search for "200" in s+4 .. s+len
    const void* p = memmem(s + 4, len - 4, "200", 3);
    return p != nullptr;
}

// substring "An"
static inline bool contains_An(const char* s, size_t len) {
    if (len < 2) return false;
    return memmem(s, len, "An", 2) != nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];
    mkdir(rdir.c_str(), 0755);

    GENDB_PHASE("total");

    // ---------- mmap all needed columns ----------
    // Dim tables
    MmapColumn<uint64_t> cct_kind_off, it_info_off, rt_role_off, kw_off;
    MmapColumn<char>     cct_kind_dat, it_info_dat, rt_role_dat, kw_dat;
    MmapColumn<int32_t>  cct_id, it_id_col, rt_id, kw_id_col;

    // char_name
    MmapColumn<uint64_t> chn_name_off;
    MmapColumn<char>     chn_name_dat;
    MmapColumn<int32_t>  chn_id;

    // company_name
    MmapColumn<uint16_t> cn_cc_codes; // 2-byte dict codes (file size = 2 * rows)
    MmapColumn<uint64_t> cn_dict_off;
    MmapColumn<char>     cn_dict_dat;
    MmapColumn<int32_t>  cn_id;

    // title
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;
    MmapColumn<int32_t>  t_id;
    MmapColumn<int32_t>  t_year;

    // facts
    MmapColumn<int32_t>  mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t>  cc_movie_id, cc_subject_id, cc_status_id;
    MmapColumn<int32_t>  mc_movie_id, mc_company_id;
    MmapColumn<int32_t>  mi_movie_id, mi_info_type_id;
    MmapColumn<uint64_t> mi_info_off;
    MmapColumn<char>     mi_info_dat;
    MmapColumn<int32_t>  ci_movie_id, ci_person_id, ci_person_role_id, ci_role_id;
    MmapColumn<uint64_t> ci_note_off;
    MmapColumn<char>     ci_note_dat;

    // name
    MmapColumn<uint64_t> n_name_off;
    MmapColumn<char>     n_name_dat;
    MmapColumn<uint8_t>  n_gender;       // 1 byte per row
    MmapColumn<uint64_t> n_gender_dict_off;
    MmapColumn<char>     n_gender_dict_dat;

    // aka_name, person_info
    MmapColumn<int32_t>  an_person_id;
    MmapColumn<int32_t>  pi_person_id, pi_info_type_id;

    // indexes
    MmapColumn<uint32_t> idx_mk, idx_cc, idx_mc, idx_mi, idx_ci, idx_an, idx_pi;

    {
        GENDB_PHASE("data_loading");
        cct_kind_off.open(gdir + "/comp_cast_type/kind.off");
        cct_kind_dat.open(gdir + "/comp_cast_type/kind.dat");
        cct_id.open(gdir + "/comp_cast_type/id.bin");

        it_info_off.open(gdir + "/info_type/info.off");
        it_info_dat.open(gdir + "/info_type/info.dat");
        it_id_col.open(gdir + "/info_type/id.bin");

        rt_role_off.open(gdir + "/role_type/role.off");
        rt_role_dat.open(gdir + "/role_type/role.dat");
        rt_id.open(gdir + "/role_type/id.bin");

        kw_off.open(gdir + "/keyword/keyword.off");
        kw_dat.open(gdir + "/keyword/keyword.dat");
        kw_id_col.open(gdir + "/keyword/id.bin");

        chn_name_off.open(gdir + "/char_name/name.off");
        chn_name_dat.open(gdir + "/char_name/name.dat");
        chn_id.open(gdir + "/char_name/id.bin");

        cn_cc_codes.open(gdir + "/company_name/country_code.bin");
        cn_dict_off.open(gdir + "/company_name/country_code.dict.off");
        cn_dict_dat.open(gdir + "/company_name/country_code.dict.dat");
        cn_id.open(gdir + "/company_name/id.bin");

        t_title_off.open(gdir + "/title/title.off");
        t_title_dat.open(gdir + "/title/title.dat");
        t_id.open(gdir + "/title/id.bin");
        t_year.open(gdir + "/title/production_year.bin");

        mk_movie_id.open(gdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");

        cc_movie_id.open(gdir + "/complete_cast/movie_id.bin");
        cc_subject_id.open(gdir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gdir + "/complete_cast/status_id.bin");

        mc_movie_id.open(gdir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gdir + "/movie_companies/company_id.bin");

        mi_movie_id.open(gdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gdir + "/movie_info/info.off");
        mi_info_dat.open(gdir + "/movie_info/info.dat");

        ci_movie_id.open(gdir + "/cast_info/movie_id.bin");
        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_role_id.open(gdir + "/cast_info/role_id.bin");
        ci_note_off.open(gdir + "/cast_info/note.off");
        ci_note_dat.open(gdir + "/cast_info/note.dat");

        n_name_off.open(gdir + "/name/name.off");
        n_name_dat.open(gdir + "/name/name.dat");
        n_gender.open(gdir + "/name/gender.bin");
        n_gender_dict_off.open(gdir + "/name/gender.dict.off");
        n_gender_dict_dat.open(gdir + "/name/gender.dict.dat");

        an_person_id.open(gdir + "/aka_name/person_id.bin");
        pi_person_id.open(gdir + "/person_info/person_id.bin");
        pi_info_type_id.open(gdir + "/person_info/info_type_id.bin");

        idx_mk.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        idx_cc.open(gdir + "/_idx/complete_cast__movie_id__offsets.bin");
        idx_mc.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");
        idx_mi.open(gdir + "/_idx/movie_info__movie_id__offsets.bin");
        idx_ci.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
        idx_an.open(gdir + "/_idx/aka_name__person_id__offsets.bin");
        idx_pi.open(gdir + "/_idx/person_info__person_id__offsets.bin");
    }

    // ---------- resolve dim ids ----------
    int32_t cast_id = -1, verified_id = -1;
    int32_t it_id = -1, it3_id = -1;
    int32_t actress_id = -1;
    int32_t cam_kw_id = -1;
    int32_t f_code = -1;
    uint16_t us_code = 0xFFFF;

    {
        // comp_cast_type
        for (size_t i = 0; i < cct_id.size(); i++) {
            size_t s = cct_kind_off[i], e = cct_kind_off[i + 1];
            size_t len = e - s;
            const char* p = cct_kind_dat.data + s;
            if (len == 4 && std::memcmp(p, "cast", 4) == 0) cast_id = cct_id[i];
            else if (len == 17 && std::memcmp(p, "complete+verified", 17) == 0) verified_id = cct_id[i];
        }
        // info_type
        for (size_t i = 0; i < it_id_col.size(); i++) {
            size_t s = it_info_off[i], e = it_info_off[i + 1];
            size_t len = e - s;
            const char* p = it_info_dat.data + s;
            if (len == 13 && std::memcmp(p, "release dates", 13) == 0) it_id = it_id_col[i];
            else if (len == 6 && std::memcmp(p, "height", 6) == 0) it3_id = it_id_col[i];
        }
        // role_type
        for (size_t i = 0; i < rt_id.size(); i++) {
            size_t s = rt_role_off[i], e = rt_role_off[i + 1];
            size_t len = e - s;
            const char* p = rt_role_dat.data + s;
            if (len == 7 && std::memcmp(p, "actress", 7) == 0) actress_id = rt_id[i];
        }
        // keyword
        for (size_t i = 0; i < kw_id_col.size(); i++) {
            size_t s = kw_off[i], e = kw_off[i + 1];
            size_t len = e - s;
            const char* p = kw_dat.data + s;
            if (len == 18 && std::memcmp(p, "computer-animation", 18) == 0) {
                cam_kw_id = kw_id_col[i];
                break;
            }
        }
        // gender dict: find 'f'
        size_t n_codes = (n_gender_dict_off.size() ? n_gender_dict_off.size() - 1 : 0);
        for (size_t i = 0; i < n_codes; i++) {
            size_t s = n_gender_dict_off[i], e = n_gender_dict_off[i + 1];
            if ((e - s) == 1 && n_gender_dict_dat.data[s] == 'f') { f_code = (int32_t)(i + 1); break; }
        }
        // country_code dict: find '[us]'
        size_t n_cc = (cn_dict_off.size() ? cn_dict_off.size() - 1 : 0);
        for (size_t i = 0; i < n_cc; i++) {
            size_t s = cn_dict_off[i], e = cn_dict_off[i + 1];
            if ((e - s) == 4 && std::memcmp(cn_dict_dat.data + s, "[us]", 4) == 0) {
                us_code = (uint16_t)(i + 1);
                break;
            }
        }
    }

    if (cast_id < 0 || verified_id < 0 || it_id < 0 || it3_id < 0 ||
        actress_id < 0 || cam_kw_id < 0 || f_code < 0 || us_code == 0xFFFF) {
        std::fprintf(stderr, "Failed to resolve dim ids: cast=%d verified=%d it=%d it3=%d actress=%d cam=%d f=%d us=%u\n",
                     cast_id, verified_id, it_id, it3_id, actress_id, cam_kw_id, f_code, us_code);
        return 2;
    }

    // ---------- build chn_set (char_name id where name == 'Queen') ----------
    std::unordered_set<int32_t> chn_set;
    {
        for (size_t i = 0; i < chn_id.size(); i++) {
            size_t s = chn_name_off[i], e = chn_name_off[i + 1];
            if ((e - s) == 5 && std::memcmp(chn_name_dat.data + s, "Queen", 5) == 0) {
                chn_set.insert(chn_id[i]);
            }
        }
    }

    // ---------- build cn_us_set (company_name id with country_code='[us]') ----------
    // bitset on company id (1..234997)
    std::vector<uint8_t> cn_us_bitset(cn_id.size() + 2, 0);
    {
        for (size_t i = 0; i < cn_id.size(); i++) {
            if (cn_cc_codes[i] == us_code) {
                int32_t id = cn_id[i];
                if (id > 0 && (size_t)id < cn_us_bitset.size()) cn_us_bitset[id] = 1;
            }
        }
    }

    // ---------- driver: scan title for 'Shrek 2' ----------
    std::vector<std::pair<int32_t, uint32_t>> drivers; // (movie_id, title_row_idx)
    {
        GENDB_PHASE("driver_scan");
        size_t n = t_id.size();
        for (size_t i = 0; i < n; i++) {
            size_t s = t_title_off[i], e = t_title_off[i + 1];
            if ((e - s) != 7) continue;
            if (std::memcmp(t_title_dat.data + s, "Shrek 2", 7) != 0) continue;
            int32_t y = t_year[i];
            if (y < 2000 || y > 2005) continue;
            drivers.push_back({t_id[i], (uint32_t)i});
        }
    }

    // ci.note literals
    static const char* NOTE_LITS[3] = {"(voice)", "(voice) (uncredited)", "(voice: English version)"};
    static const size_t NOTE_LENS[3] = {7, 20, 24};

    auto note_in_set = [&](size_t s, size_t e) -> bool {
        size_t L = e - s;
        const char* p = ci_note_dat.data + s;
        if (L == 7 && std::memcmp(p, NOTE_LITS[0], 7) == 0) return true;
        if (L == 20 && std::memcmp(p, NOTE_LITS[1], 20) == 0) return true;
        if (L == 24 && std::memcmp(p, NOTE_LITS[2], 24) == 0) return true;
        return false;
    };

    // ---------- main pipeline ----------
    std::string best_chn = "";   // "Queen" only (since chn.name='Queen' is filter)
    std::string best_n   = "";   // actress name
    std::string best_t   = "";   // movie title
    bool any_match = false;

    {
        GENDB_PHASE("main_scan");
        for (auto& dr : drivers) {
            int32_t mid = dr.first;
            uint32_t trow = dr.second;

            // probe mk
            uint32_t lo, hi;
            lo = idx_mk[mid]; hi = idx_mk[mid + 1];
            bool ok = false;
            for (uint32_t r = lo; r < hi; r++) {
                if (mk_keyword_id[r] == cam_kw_id) { ok = true; break; }
            }
            if (!ok) continue;

            // probe cc
            lo = idx_cc[mid]; hi = idx_cc[mid + 1];
            ok = false;
            for (uint32_t r = lo; r < hi; r++) {
                if (cc_subject_id[r] == cast_id && cc_status_id[r] == verified_id) { ok = true; break; }
            }
            if (!ok) continue;

            // probe mc
            lo = idx_mc[mid]; hi = idx_mc[mid + 1];
            ok = false;
            for (uint32_t r = lo; r < hi; r++) {
                int32_t cid = mc_company_id[r];
                if (cid > 0 && (size_t)cid < cn_us_bitset.size() && cn_us_bitset[cid]) { ok = true; break; }
            }
            if (!ok) continue;

            // probe mi
            lo = idx_mi[mid]; hi = idx_mi[mid + 1];
            ok = false;
            for (uint32_t r = lo; r < hi; r++) {
                if (mi_info_type_id[r] != it_id) continue;
                size_t s = mi_info_off[r], e = mi_info_off[r + 1];
                if (like_usa_200(mi_info_dat.data + s, e - s)) { ok = true; break; }
            }
            if (!ok) continue;

            // probe ci → collect candidate persons
            lo = idx_ci[mid]; hi = idx_ci[mid + 1];
            std::vector<int32_t> cand_persons;
            for (uint32_t r = lo; r < hi; r++) {
                if (ci_role_id[r] != actress_id) continue;
                int32_t prr = ci_person_role_id[r];
                if (chn_set.find(prr) == chn_set.end()) continue;
                size_t ns = ci_note_off[r], ne = ci_note_off[r + 1];
                if (!note_in_set(ns, ne)) continue;
                cand_persons.push_back(ci_person_id[r]);
            }
            if (cand_persons.empty()) continue;

            // dedupe candidates
            std::sort(cand_persons.begin(), cand_persons.end());
            cand_persons.erase(std::unique(cand_persons.begin(), cand_persons.end()), cand_persons.end());

            // per-person filters
            for (int32_t pid : cand_persons) {
                size_t prow = (size_t)(pid - 1); // name dense PK
                if (prow >= n_gender.size()) continue;
                if ((int32_t)n_gender[prow] != f_code) continue;
                size_t ns = n_name_off[prow], ne = n_name_off[prow + 1];
                size_t nL = ne - ns;
                const char* npstr = n_name_dat.data + ns;
                if (!contains_An(npstr, nL)) continue;

                // aka_name existence
                uint32_t alo = idx_an[pid], ahi = idx_an[pid + 1];
                if (alo == ahi) continue;

                // person_info: has any row with info_type_id == it3_id
                uint32_t plo = idx_pi[pid], phi = idx_pi[pid + 1];
                bool pi_ok = false;
                for (uint32_t r = plo; r < phi; r++) {
                    if (pi_info_type_id[r] == it3_id) { pi_ok = true; break; }
                }
                if (!pi_ok) continue;

                // match — update MINs
                std::string nname(npstr, nL);
                size_t ts = t_title_off[trow], te = t_title_off[trow + 1];
                std::string tname(t_title_dat.data + ts, te - ts);
                std::string cname = "Queen";

                if (!any_match) {
                    best_chn = cname;
                    best_n = nname;
                    best_t = tname;
                    any_match = true;
                } else {
                    if (cname < best_chn) best_chn = cname;
                    if (nname < best_n)   best_n   = nname;
                    if (tname < best_t)   best_t   = tname;
                }
            }
        }
    }

    // ---------- output CSV ----------
    {
        GENDB_PHASE("output");
        std::string outpath = rdir + "/Q29b.csv";
        FILE* f = std::fopen(outpath.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Cannot open %s\n", outpath.c_str()); return 3; }
        std::fprintf(f, "voiced_char,voicing_actress,voiced_animation\n");
        if (any_match) {
            auto emit = [&](const std::string& s) {
                bool need_quote = s.find(',') != std::string::npos || s.find('"') != std::string::npos;
                if (need_quote) {
                    std::fputc('"', f);
                    for (char c : s) {
                        if (c == '"') std::fputc('"', f);
                        std::fputc(c, f);
                    }
                    std::fputc('"', f);
                } else {
                    std::fputs(s.c_str(), f);
                }
            };
            emit(best_chn); std::fputc(',', f);
            emit(best_n);   std::fputc(',', f);
            emit(best_t);   std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
