// Q29c — voiced animation characters (computer-animation, 2000-2010, US, female actresses)
// Driver: movie_keyword CSR(cam_kw_id). Outputs MIN(chn.name), MIN(n.name), MIN(t.title).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <unordered_map>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;
namespace fs = std::filesystem;

static constexpr int32_t NULL_INT = INT32_MIN;

// memmem: substring search
static inline const void* my_memmem(const void* hay, size_t hl,
                                    const void* nd, size_t nl) {
    if (nl == 0) return hay;
    if (hl < nl) return nullptr;
    return memmem(hay, hl, nd, nl);
}

// Resolve a varlen dim string-id, scanning .off/.dat for a literal.
static int32_t resolve_varlen_id(const int64_t* off, const char* dat,
                                 size_t n_rows, const int32_t* id_col,
                                 const std::string& needle) {
    for (size_t r = 0; r < n_rows; ++r) {
        int64_t s = off[r], e = off[r+1];
        size_t len = (size_t)(e - s);
        if (len == needle.size() && memcmp(dat + s, needle.data(), len) == 0) {
            return id_col[r];
        }
    }
    return -1;
}

// Resolve a dict code by literal.
static int32_t resolve_dict_code(const int64_t* doff, const char* ddat,
                                 size_t n_dict, const std::string& needle) {
    // dict code i references entry i-1; code 0 = NULL
    for (size_t i = 0; i < n_dict; ++i) {
        int64_t s = doff[i], e = doff[i+1];
        size_t len = (size_t)(e - s);
        if (len == needle.size() && memcmp(ddat + s, needle.data(), len) == 0) {
            return (int32_t)(i + 1);
        }
    }
    return -1;
}

// Update MIN string given candidate bytes. Returns true if updated.
static inline bool min_update(std::string& cur, const char* p, size_t len) {
    std::string_view sv(p, len);
    if (cur.empty()) { cur.assign(p, len); return true; }
    if (sv < std::string_view(cur)) { cur.assign(p, len); return true; }
    return false;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // --------------------------- DATA LOADING ---------------------------
    MmapColumn<int64_t> cct_kind_off, it_info_off, rt_role_off, kw_kw_off;
    MmapColumn<char>    cct_kind_dat, it_info_dat, rt_role_dat, kw_kw_dat;
    MmapColumn<int32_t> cct_id, it_id, rt_id, kw_id;

    MmapColumn<int32_t> cn_id;
    MmapColumn<int8_t>  cn_cc_code;
    MmapColumn<int64_t> cn_cc_doff;
    MmapColumn<char>    cn_cc_ddat;

    MmapColumn<int8_t>  n_gender_code;
    MmapColumn<int64_t> n_gender_doff;
    MmapColumn<char>    n_gender_ddat;
    MmapColumn<int64_t> n_name_off;
    MmapColumn<char>    n_name_dat;

    MmapColumn<int64_t> chn_name_off;
    MmapColumn<char>    chn_name_dat;

    MmapColumn<int32_t> t_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t> mk_kw_off, mk_kw_rowids;

    MmapColumn<int32_t> cc_movie_id, cc_subject_id, cc_status_id;
    MmapColumn<int32_t> cc_off;

    MmapColumn<int32_t> mc_movie_id, mc_company_id;
    MmapColumn<int32_t> mc_off;

    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int32_t> mi_off;

    MmapColumn<int32_t> ci_movie_id, ci_person_id, ci_person_role_id, ci_role_id;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;
    MmapColumn<int32_t> ci_off;

    MmapColumn<int32_t> an_off;

    MmapColumn<int32_t> pi_info_type_id;
    MmapColumn<int32_t> pi_off;

    {
        GENDB_PHASE("data_loading");
        // dim tables
        cct_kind_off.open(gendb_dir + "/comp_cast_type/kind.off");
        cct_kind_dat.open(gendb_dir + "/comp_cast_type/kind.dat");
        cct_id.open(gendb_dir + "/comp_cast_type/id.bin");

        it_info_off.open(gendb_dir + "/info_type/info.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");
        it_id.open(gendb_dir + "/info_type/id.bin");

        rt_role_off.open(gendb_dir + "/role_type/role.off");
        rt_role_dat.open(gendb_dir + "/role_type/role.dat");
        rt_id.open(gendb_dir + "/role_type/id.bin");

        kw_kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_kw_dat.open(gendb_dir + "/keyword/keyword.dat");
        kw_id.open(gendb_dir + "/keyword/id.bin");

        cn_id.open(gendb_dir + "/company_name/id.bin");
        cn_cc_code.open(gendb_dir + "/company_name/country_code.bin");
        cn_cc_doff.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_cc_ddat.open(gendb_dir + "/company_name/country_code.dict.dat");

        n_gender_code.open(gendb_dir + "/name/gender.bin");
        n_gender_doff.open(gendb_dir + "/name/gender.dict.off");
        n_gender_ddat.open(gendb_dir + "/name/gender.dict.dat");
        n_name_off.open(gendb_dir + "/name/name.off");
        n_name_dat.open(gendb_dir + "/name/name.dat");

        chn_name_off.open(gendb_dir + "/char_name/name.off");
        chn_name_dat.open(gendb_dir + "/char_name/name.dat");

        t_year.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");
        mk_kw_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");

        cc_movie_id.open(gendb_dir + "/complete_cast/movie_id.bin");
        cc_subject_id.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");
        cc_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");

        mc_movie_id.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");

        mi_movie_id.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");

        ci_movie_id.open(gendb_dir + "/cast_info/movie_id.bin");
        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");
        ci_person_role_id.open(gendb_dir + "/cast_info/person_role_id.bin");
        ci_role_id.open(gendb_dir + "/cast_info/role_id.bin");
        ci_note_off.open(gendb_dir + "/cast_info/note.off");
        ci_note_dat.open(gendb_dir + "/cast_info/note.dat");
        ci_off.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");

        an_off.open(gendb_dir + "/_idx/aka_name__person_id__offsets.bin");

        pi_info_type_id.open(gendb_dir + "/person_info/info_type_id.bin");
        pi_off.open(gendb_dir + "/_idx/person_info__person_id__offsets.bin");
    }

    // ---------------------- RESOLVE DIM CODES ----------------------
    int32_t cct_cast_id = resolve_varlen_id(cct_kind_off.data, cct_kind_dat.data,
                                            cct_kind_off.count - 1, cct_id.data, "cast");
    int32_t cct_verified_id = resolve_varlen_id(cct_kind_off.data, cct_kind_dat.data,
                                                cct_kind_off.count - 1, cct_id.data, "complete+verified");
    int32_t it_release_id = resolve_varlen_id(it_info_off.data, it_info_dat.data,
                                              it_info_off.count - 1, it_id.data, "release dates");
    int32_t it3_trivia_id = resolve_varlen_id(it_info_off.data, it_info_dat.data,
                                              it_info_off.count - 1, it_id.data, "trivia");
    int32_t rt_actress_id = resolve_varlen_id(rt_role_off.data, rt_role_dat.data,
                                              rt_role_off.count - 1, rt_id.data, "actress");
    int32_t cam_kw_id = resolve_varlen_id(kw_kw_off.data, kw_kw_dat.data,
                                          kw_kw_off.count - 1, kw_id.data, "computer-animation");

    int32_t us_code = resolve_dict_code(cn_cc_doff.data, cn_cc_ddat.data,
                                        cn_cc_doff.count - 1, "[us]");
    int32_t f_code  = resolve_dict_code(n_gender_doff.data, n_gender_ddat.data,
                                        n_gender_doff.count - 1, "f");

    if (cct_cast_id < 0 || cct_verified_id < 0 || it_release_id < 0 ||
        it3_trivia_id < 0 || rt_actress_id < 0 || cam_kw_id < 0 ||
        us_code < 0 || f_code < 0) {
        fprintf(stderr, "dim resolution failed: cct_cast=%d cct_verified=%d it_release=%d it_trivia=%d rt_actress=%d cam_kw=%d us=%d f=%d\n",
                cct_cast_id, cct_verified_id, it_release_id, it3_trivia_id,
                rt_actress_id, cam_kw_id, us_code, f_code);
        return 2;
    }

    // ---------------------- BUILD cn_us_set ----------------------
    // bitset over cn.id (dense 1..N for company_name)
    size_t cn_rows = cn_id.count;
    int32_t max_cn_id = 0;
    for (size_t i = 0; i < cn_rows; ++i) if (cn_id[i] > max_cn_id) max_cn_id = cn_id[i];
    std::vector<uint8_t> cn_us_set(max_cn_id + 2, 0);
    {
        GENDB_PHASE("build_cn_us_set");
        for (size_t i = 0; i < cn_rows; ++i) {
            if ((int32_t)cn_cc_code[i] == us_code) cn_us_set[cn_id[i]] = 1;
        }
    }

    // 4 note literals
    static const char* NOTE_LITS[4] = {
        "(voice)",
        "(voice: Japanese version)",
        "(voice) (uncredited)",
        "(voice: English version)"
    };
    static const size_t NOTE_LENS[4] = { 7, 25, 20, 24 };

    auto note_matches = [&](const char* p, size_t len) -> bool {
        // very small set; bucket by length first
        if (len == 7  && memcmp(p, NOTE_LITS[0], 7)  == 0) return true;
        if (len == 25 && memcmp(p, NOTE_LITS[1], 25) == 0) return true;
        if (len == 20 && memcmp(p, NOTE_LITS[2], 20) == 0) return true;
        if (len == 24 && memcmp(p, NOTE_LITS[3], 24) == 0) return true;
        return false;
    };

    // Cache per-person eligibility:
    //   0 unknown, 1 eligible, 2 ineligible
    std::vector<uint8_t> person_state(n_name_off.count, 0); // n_name_off.count == n_rows + 1; ok
    // Note: n_rows is name table rows; index by person_id which is 1..n_rows.

    // ---------------------- MAIN DRIVING SCAN ----------------------
    std::string min_chn, min_name, min_title;

    size_t lo = mk_kw_off[cam_kw_id], hi = mk_kw_off[cam_kw_id + 1];
    // Aux limits
    size_t mc_off_n = mc_off.count;
    size_t mi_off_n = mi_off.count;
    size_t cc_off_n = cc_off.count;
    size_t ci_off_n = ci_off.count;
    size_t an_off_n = an_off.count;
    size_t pi_off_n = pi_off.count;
    size_t name_rows = n_name_off.count - 1;

    size_t n_candidates = 0;
    size_t n_after_year = 0;
    size_t n_after_cc = 0;
    size_t n_after_mc = 0;
    size_t n_after_mi = 0;
    size_t n_final = 0;

    {
        GENDB_PHASE("main_scan");
        for (size_t k = lo; k < hi; ++k) {
            int32_t mk_row = mk_kw_rowids[k];
            int32_t t_id_val = mk_movie_id[mk_row];
            ++n_candidates;

            // 1. year filter (title row index = t_id - 1; id dense 1..N)
            // Title.id dense 1..N so t_row = t_id_val - 1
            int32_t t_row = t_id_val - 1;
            if (t_row < 0 || (size_t)t_row >= t_year.count) continue;
            int32_t year = t_year[t_row];
            if (year == NULL_INT || year < 2000 || year > 2010) continue;
            ++n_after_year;

            // 2. complete_cast: subject==cast_id && status==verified_id
            if ((size_t)t_id_val + 1 >= cc_off_n) continue;
            int32_t cc_lo = cc_off[t_id_val], cc_hi = cc_off[t_id_val + 1];
            bool cc_ok = false;
            for (int32_t r = cc_lo; r < cc_hi; ++r) {
                if (cc_subject_id[r] == cct_cast_id && cc_status_id[r] == cct_verified_id) {
                    cc_ok = true; break;
                }
            }
            if (!cc_ok) continue;
            ++n_after_cc;

            // 3. movie_companies: company_id in cn_us_set
            if ((size_t)t_id_val + 1 >= mc_off_n) continue;
            int32_t mc_lo = mc_off[t_id_val], mc_hi = mc_off[t_id_val + 1];
            bool mc_ok = false;
            for (int32_t r = mc_lo; r < mc_hi; ++r) {
                int32_t cid = mc_company_id[r];
                if (cid > 0 && cid <= max_cn_id && cn_us_set[cid]) { mc_ok = true; break; }
            }
            if (!mc_ok) continue;
            ++n_after_mc;

            // 4. movie_info: info_type_id==it_release && (info LIKE 'Japan:%200%' OR 'USA:%200%')
            if ((size_t)t_id_val + 1 >= mi_off_n) continue;
            int32_t mi_lo = mi_off[t_id_val], mi_hi = mi_off[t_id_val + 1];
            bool mi_ok = false;
            for (int32_t r = mi_lo; r < mi_hi; ++r) {
                if (mi_info_type_id[r] != it_release_id) continue;
                int64_t s = mi_info_off[r], e = mi_info_off[r+1];
                size_t len = (size_t)(e - s);
                if (len == 0) continue; // NULL
                const char* p = mi_info_dat.data + s;
                bool prefix_jp = (len >= 6 && memcmp(p, "Japan:", 6) == 0);
                bool prefix_us = (len >= 4 && memcmp(p, "USA:", 4) == 0);
                if (!prefix_jp && !prefix_us) continue;
                size_t skip = prefix_jp ? 6 : 4;
                if (my_memmem(p + skip, len - skip, "200", 3)) {
                    mi_ok = true; break;
                }
            }
            if (!mi_ok) continue;
            ++n_after_mi;

            // 5. cast_info: role_id==actress && note in 4-set && person_role_id != NULL
            if ((size_t)t_id_val + 1 >= ci_off_n) continue;
            int32_t ci_lo = ci_off[t_id_val], ci_hi = ci_off[t_id_val + 1];

            // Title for this movie (lazy compare placeholder)
            int64_t tts = t_title_off[t_row], tte = t_title_off[t_row + 1];
            const char* t_title_p = t_title_dat.data + tts;
            size_t t_title_len = (size_t)(tte - tts);

            bool any_hit_this_movie = false;
            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                if (ci_role_id[r] != rt_actress_id) continue;
                int32_t prid = ci_person_role_id[r];
                if (prid == NULL_INT) continue;
                int64_t ns = ci_note_off[r], ne = ci_note_off[r+1];
                size_t nlen = (size_t)(ne - ns);
                if (nlen == 0) continue;
                if (!note_matches(ci_note_dat.data + ns, nlen)) continue;

                int32_t p_id = ci_person_id[r];
                if (p_id <= 0 || (size_t)p_id > name_rows) continue;

                // Per-person eligibility cache
                uint8_t st = person_state[p_id];
                if (st == 2) continue;
                if (st == 0) {
                    // Compute eligibility
                    // gender == f_code
                    if ((int32_t)n_gender_code[p_id - 1] != f_code) { person_state[p_id] = 2; continue; }
                    // name LIKE '%An%'
                    int64_t nns = n_name_off[p_id - 1], nne = n_name_off[p_id];
                    size_t nnlen = (size_t)(nne - nns);
                    if (!my_memmem(n_name_dat.data + nns, nnlen, "An", 2)) { person_state[p_id] = 2; continue; }
                    // aka_name existence on person_id
                    if ((size_t)p_id + 1 >= an_off_n) { person_state[p_id] = 2; continue; }
                    if (an_off[p_id + 1] <= an_off[p_id]) { person_state[p_id] = 2; continue; }
                    // person_info existence with info_type_id == it3_trivia_id
                    if ((size_t)p_id + 1 >= pi_off_n) { person_state[p_id] = 2; continue; }
                    int32_t pi_lo = pi_off[p_id], pi_hi = pi_off[p_id + 1];
                    bool pi_ok = false;
                    for (int32_t pr = pi_lo; pr < pi_hi; ++pr) {
                        if (pi_info_type_id[pr] == it3_trivia_id) { pi_ok = true; break; }
                    }
                    if (!pi_ok) { person_state[p_id] = 2; continue; }
                    person_state[p_id] = 1;
                }

                // Eligible — update MINs
                // chn.name via person_role_id (chn.id dense 1..N? assume yes for char_name)
                int32_t chn_row = prid - 1;
                if (chn_row < 0 || (size_t)chn_row + 1 >= chn_name_off.count) continue;
                int64_t cs = chn_name_off[chn_row], ce = chn_name_off[chn_row + 1];
                const char* chn_p = chn_name_dat.data + cs;
                size_t chn_len = (size_t)(ce - cs);
                if (chn_len == 0) continue; // NULL char name (shouldn't but skip)

                // n.name
                int64_t nns = n_name_off[p_id - 1], nne = n_name_off[p_id];
                const char* n_p = n_name_dat.data + nns;
                size_t n_len = (size_t)(nne - nns);

                min_update(min_chn, chn_p, chn_len);
                min_update(min_name, n_p, n_len);
                if (!any_hit_this_movie) {
                    min_update(min_title, t_title_p, t_title_len);
                    any_hit_this_movie = true;
                }
                ++n_final;
            }
        }
    }

    fprintf(stderr, "[STATS] candidates=%zu after_year=%zu after_cc=%zu after_mc=%zu after_mi=%zu finals=%zu\n",
            n_candidates, n_after_year, n_after_cc, n_after_mc, n_after_mi, n_final);

    // ---------------------- OUTPUT ----------------------
    auto csv_escape = [](const std::string& s) -> std::string {
        bool needs_quote = false;
        for (char c : s) {
            if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
        }
        if (!needs_quote) return s;
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            if (c == '"') out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    };
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q29c.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "voiced_char,voicing_actress,voiced_animation\n");
        std::fprintf(f, "%s,%s,%s\n",
                     csv_escape(min_chn).c_str(),
                     csv_escape(min_name).c_str(),
                     csv_escape(min_title).c_str());
        std::fclose(f);
    }

    return 0;
}
