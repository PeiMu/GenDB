// Q24a generated implementation
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;
using std::string_view;

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: q24a <gendb_dir> <results_dir>\n");
        return 1;
    }
    std::string g(argv[1]);
    std::string r(argv[2]);
    std::filesystem::create_directories(r);

    // ----- Mmap all data -----
    MmapColumn<char> name_dat, char_name_dat, title_dat, info_type_dat, role_type_dat, keyword_dat, cast_note_dat, movie_info_dat;
    MmapColumn<int64_t> name_off, char_name_off, title_off, info_type_off, role_type_off, keyword_off, cast_note_off, mi_info_off;
    MmapColumn<int8_t> gender;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int32_t> title_year;
    MmapColumn<int32_t> mk_off, mc_off, mi_off, ci_off, aka_off;
    MmapColumn<int32_t> mk_keyword_id, mc_company_id, mi_info_type_id, ci_role_id, ci_person_id, ci_person_role_id;
    MmapColumn<int32_t> rt_id_col, it_id_col, kw_id_col;
    MmapColumn<int64_t> gd_off; MmapColumn<char> gd_dat;
    MmapColumn<int64_t> cd_off; MmapColumn<char> cd_dat;

    {
        GENDB_PHASE("data_loading");
        name_dat.open(g + "/name/name.dat");
        name_off.open(g + "/name/name.off");
        gender.open(g + "/name/gender.bin");

        char_name_dat.open(g + "/char_name/name.dat");
        char_name_off.open(g + "/char_name/name.off");

        title_dat.open(g + "/title/title.dat");
        title_off.open(g + "/title/title.off");
        title_year.open(g + "/title/production_year.bin");

        cn_country_code.open(g + "/company_name/country_code.bin");

        info_type_dat.open(g + "/info_type/info.dat");
        info_type_off.open(g + "/info_type/info.off");
        it_id_col.open(g + "/info_type/id.bin");

        role_type_dat.open(g + "/role_type/role.dat");
        role_type_off.open(g + "/role_type/role.off");
        rt_id_col.open(g + "/role_type/id.bin");

        keyword_dat.open(g + "/keyword/keyword.dat");
        keyword_off.open(g + "/keyword/keyword.off");
        kw_id_col.open(g + "/keyword/id.bin");

        cast_note_dat.open(g + "/cast_info/note.dat");
        cast_note_off.open(g + "/cast_info/note.off");

        movie_info_dat.open(g + "/movie_info/info.dat");
        mi_info_off.open(g + "/movie_info/info.off");
        mi_info_type_id.open(g + "/movie_info/info_type_id.bin");

        mk_keyword_id.open(g + "/movie_keyword/keyword_id.bin");
        mc_company_id.open(g + "/movie_companies/company_id.bin");
        ci_role_id.open(g + "/cast_info/role_id.bin");
        ci_person_id.open(g + "/cast_info/person_id.bin");
        ci_person_role_id.open(g + "/cast_info/person_role_id.bin");

        mk_off.open(g + "/_idx/movie_keyword__movie_id__offsets.bin");
        mc_off.open(g + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off.open(g + "/_idx/movie_info__movie_id__offsets.bin");
        ci_off.open(g + "/_idx/cast_info__movie_id__offsets.bin");
        aka_off.open(g + "/_idx/aka_name__person_id__offsets.bin");

        gd_off.open(g + "/name/gender.dict.off");
        gd_dat.open(g + "/name/gender.dict.dat");
        cd_off.open(g + "/company_name/country_code.dict.off");
        cd_dat.open(g + "/company_name/country_code.dict.dat");
    }

    // ----- Resolve dim ids and dict codes -----
    int32_t rt_id = INT32_MIN, it_id = INT32_MIN;
    int8_t  f_code = -1;
    int16_t us_code = -1;
    int32_t kw_arr[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    int kw_count = 0;
    {
        GENDB_PHASE("dim_resolve");
        // role_type 'actress'
        for (size_t i = 0; i < rt_id_col.count; i++) {
            size_t l = role_type_off[i+1] - role_type_off[i];
            if (l == 7 && memcmp(role_type_dat.data + role_type_off[i], "actress", 7) == 0) {
                rt_id = rt_id_col[i];
                break;
            }
        }
        // info_type 'release dates'
        for (size_t i = 0; i < it_id_col.count; i++) {
            size_t l = info_type_off[i+1] - info_type_off[i];
            if (l == 13 && memcmp(info_type_dat.data + info_type_off[i], "release dates", 13) == 0) {
                it_id = it_id_col[i];
                break;
            }
        }
        // keyword: hero, martial-arts, hand-to-hand-combat
        const char* tgt[3] = {"hero", "martial-arts", "hand-to-hand-combat"};
        size_t   tlen[3] = {4, 12, 19};
        for (size_t i = 0; i < kw_id_col.count && kw_count < 3; i++) {
            size_t l = keyword_off[i+1] - keyword_off[i];
            const char* p = keyword_dat.data + keyword_off[i];
            for (int t = 0; t < 3; t++) {
                if (l == tlen[t] && memcmp(p, tgt[t], l) == 0) {
                    kw_arr[kw_count++] = kw_id_col[i];
                    break;
                }
            }
        }
        // gender dict 'f'  (codes are 1-based with 0 reserved for NULL → code = dict_index + 1)
        for (size_t i = 0; i + 1 < gd_off.count; i++) {
            size_t l = gd_off[i+1] - gd_off[i];
            if (l == 1 && gd_dat.data[gd_off[i]] == 'f') {
                f_code = (int8_t)(i + 1);
                break;
            }
        }
        // country_code dict '[us]'  (codes are 1-based, 0 = NULL)
        for (size_t i = 0; i + 1 < cd_off.count; i++) {
            size_t l = cd_off[i+1] - cd_off[i];
            if (l == 4 && memcmp(cd_dat.data + cd_off[i], "[us]", 4) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }
    }
    if (rt_id == INT32_MIN || it_id == INT32_MIN || f_code < 0 || us_code < 0 || kw_count == 0) {
        std::fprintf(stderr, "missing dim values rt=%d it=%d fc=%d uc=%d kwc=%d\n",
            rt_id, it_id, (int)f_code, (int)us_code, kw_count);
        return 1;
    }

    // ----- Pre-pass: build valid_persons byte array (gender='f' AND name LIKE '%An%') -----
    size_t n_persons = gender.count;
    std::vector<uint8_t> valid_persons(n_persons, 0);
    {
        GENDB_PHASE("prepass_persons");
        const char* nd = name_dat.data;
        #pragma omp parallel for schedule(static, 16384)
        for (size_t i = 0; i < n_persons; i++) {
            if (gender[i] != f_code) continue;
            int64_t lo = name_off[i], hi = name_off[i+1];
            size_t len = (size_t)(hi - lo);
            if (len < 2) continue;
            if (memmem(nd + lo, len, "An", 2) == nullptr) continue;
            valid_persons[i] = 1;
        }
    }

    // ----- Note set (4 string literals) -----
    struct Note { const char* s; size_t l; };
    Note notes[4] = {
        {"(voice)", 7},
        {"(voice: Japanese version)", 25},
        {"(voice) (uncredited)", 20},
        {"(voice: English version)", 24}
    };
    const size_t note_min = 7, note_max = 25;

    // ----- Main scan: title driver -----
    size_t n_titles = title_year.count;

    struct LocalMin {
        std::string chn_name;
        std::string n_name;
        std::string t_title;
        bool has = false;
    };

    int n_threads = omp_get_max_threads();
    std::vector<LocalMin> tls(n_threads);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            LocalMin& lm = tls[tid];

            #pragma omp for schedule(dynamic, 16384) nowait
            for (size_t v = 1; v <= n_titles; v++) {
                int32_t y = title_year[v-1];
                if (y <= 2010) continue; // INT32_MIN < 2010 so caught

                // mk range: any keyword_id in kw_arr
                int32_t mk_lo = mk_off[v], mk_hi = mk_off[v+1];
                bool mk_ok = false;
                for (int32_t i = mk_lo; i < mk_hi; i++) {
                    int32_t kid = mk_keyword_id[i];
                    if (kid == kw_arr[0] || kid == kw_arr[1] || kid == kw_arr[2]) { mk_ok = true; break; }
                }
                if (!mk_ok) continue;

                // mc range: any company_id with us_code
                int32_t mc_lo = mc_off[v], mc_hi = mc_off[v+1];
                bool mc_ok = false;
                for (int32_t i = mc_lo; i < mc_hi; i++) {
                    int32_t cid = mc_company_id[i];
                    if (cid >= 1 && (size_t)cid <= cn_country_code.count && cn_country_code[cid-1] == us_code) {
                        mc_ok = true; break;
                    }
                }
                if (!mc_ok) continue;

                // mi range: info_type_id == it_id AND (info LIKE 'Japan:%201%' OR LIKE 'USA:%201%')
                int32_t mi_lo = mi_off[v], mi_hi = mi_off[v+1];
                bool mi_ok = false;
                for (int32_t i = mi_lo; i < mi_hi; i++) {
                    if (mi_info_type_id[i] != it_id) continue;
                    int64_t io_lo = mi_info_off[i], io_hi = mi_info_off[i+1];
                    size_t ilen = (size_t)(io_hi - io_lo);
                    if (ilen == 0) continue;
                    const char* ip = movie_info_dat.data + io_lo;
                    const char* rest = nullptr;
                    size_t rlen = 0;
                    if (ilen >= 6 && memcmp(ip, "Japan:", 6) == 0) {
                        rest = ip + 6; rlen = ilen - 6;
                    } else if (ilen >= 4 && memcmp(ip, "USA:", 4) == 0) {
                        rest = ip + 4; rlen = ilen - 4;
                    } else continue;
                    if (rlen < 3) continue;
                    if (memmem(rest, rlen, "201", 3) != nullptr) { mi_ok = true; break; }
                }
                if (!mi_ok) continue;

                // ci range: per-row filters then aggregate
                int32_t ci_lo = ci_off[v], ci_hi = ci_off[v+1];
                for (int32_t rr = ci_lo; rr < ci_hi; rr++) {
                    if (ci_role_id[rr] != rt_id) continue;
                    int32_t prid = ci_person_role_id[rr];
                    if (prid == INT32_MIN || prid < 1) continue;
                    int32_t pid = ci_person_id[rr];
                    if (pid < 1 || (size_t)pid > n_persons) continue;
                    if (!valid_persons[pid - 1]) continue;

                    // note
                    int64_t no_lo = cast_note_off[rr], no_hi = cast_note_off[rr+1];
                    size_t nlen = (size_t)(no_hi - no_lo);
                    if (nlen < note_min || nlen > note_max) continue;
                    const char* np = cast_note_dat.data + no_lo;
                    bool note_ok = false;
                    for (int k = 0; k < 4; k++) {
                        if (nlen == notes[k].l && memcmp(np, notes[k].s, nlen) == 0) {
                            note_ok = true; break;
                        }
                    }
                    if (!note_ok) continue;

                    // aka existence
                    int32_t aka_lo = aka_off[pid], aka_hi = aka_off[pid+1];
                    if (aka_lo >= aka_hi) continue;

                    // Bounds for chn
                    if ((size_t)prid > char_name_off.count - 1) continue;

                    // Fetch and update mins
                    int64_t cn_lo_b = char_name_off[prid - 1];
                    int64_t cn_hi_b = char_name_off[prid];
                    string_view chn_sv(char_name_dat.data + cn_lo_b, (size_t)(cn_hi_b - cn_lo_b));

                    int64_t nm_lo_b = name_off[pid - 1];
                    int64_t nm_hi_b = name_off[pid];
                    string_view n_sv(name_dat.data + nm_lo_b, (size_t)(nm_hi_b - nm_lo_b));

                    int64_t t_lo_b = title_off[v - 1];
                    int64_t t_hi_b = title_off[v];
                    string_view t_sv(title_dat.data + t_lo_b, (size_t)(t_hi_b - t_lo_b));

                    if (!lm.has) {
                        lm.chn_name.assign(chn_sv);
                        lm.n_name.assign(n_sv);
                        lm.t_title.assign(t_sv);
                        lm.has = true;
                    } else {
                        if (chn_sv < string_view(lm.chn_name)) lm.chn_name.assign(chn_sv);
                        if (n_sv   < string_view(lm.n_name))   lm.n_name.assign(n_sv);
                        if (t_sv   < string_view(lm.t_title)) lm.t_title.assign(t_sv);
                    }
                }
            }
        }
    }

    // ----- Reduce per-thread mins -----
    std::string g_chn, g_n, g_t;
    bool has = false;
    for (auto& lm : tls) {
        if (!lm.has) continue;
        if (!has) {
            g_chn = lm.chn_name;
            g_n   = lm.n_name;
            g_t   = lm.t_title;
            has = true;
        } else {
            if (lm.chn_name < g_chn) g_chn = lm.chn_name;
            if (lm.n_name   < g_n)   g_n   = lm.n_name;
            if (lm.t_title  < g_t)   g_t   = lm.t_title;
        }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::string out_path = r + "/Q24a.csv";
        FILE* f = fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        fprintf(f, "voiced_char_name,voicing_actress_name,voiced_action_movie_jap_eng\n");
        auto wcsv = [&](const std::string& s) {
            bool need = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
            }
            if (need) {
                fputc('"', f);
                for (char c : s) {
                    if (c == '"') fputc('"', f);
                    fputc(c, f);
                }
                fputc('"', f);
            } else {
                fwrite(s.data(), 1, s.size(), f);
            }
        };
        if (has) {
            wcsv(g_chn); fputc(',', f);
            wcsv(g_n);   fputc(',', f);
            wcsv(g_t);   fputc('\n', f);
        }
        fclose(f);
    }

    return 0;
}
