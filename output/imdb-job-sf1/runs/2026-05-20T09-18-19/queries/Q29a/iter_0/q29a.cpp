// Q29a — voiced animation characters (Shrek 2 driver, Queen char, female 'An' actresses)
// Driver: title varlen scan -> 'Shrek 2' candidate t_ids. Outputs MIN(chn.name), MIN(n.name), MIN(t.title).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;
namespace fs = std::filesystem;

static constexpr int32_t NULL_INT = INT32_MIN;

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

// Resolve a dict code by literal. dict code i (1-indexed) references entry i-1; code 0 = NULL.
static int32_t resolve_dict_code(const int64_t* doff, const char* ddat,
                                 size_t n_dict, const std::string& needle) {
    for (size_t i = 0; i < n_dict; ++i) {
        int64_t s = doff[i], e = doff[i+1];
        size_t len = (size_t)(e - s);
        if (len == needle.size() && memcmp(ddat + s, needle.data(), len) == 0) {
            return (int32_t)(i + 1);
        }
    }
    return -1;
}

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

    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mk_off;

    MmapColumn<int32_t> cc_subject_id, cc_status_id;
    MmapColumn<int32_t> cc_off;

    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_off;

    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int32_t> mi_off;

    MmapColumn<int32_t> ci_person_id, ci_person_role_id, ci_role_id;
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

        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");
        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");

        cc_subject_id.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");
        cc_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");

        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");

        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");

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

    // ---------------------- BUILD chn_set (Queen) ----------------------
    std::vector<int32_t> chn_set;
    {
        GENDB_PHASE("build_chn_set");
        size_t chn_rows = chn_name_off.count - 1;
        const int64_t* off = chn_name_off.data;
        const char* dat = chn_name_dat.data;
        for (size_t r = 0; r < chn_rows; ++r) {
            int64_t s = off[r], e = off[r+1];
            size_t len = (size_t)(e - s);
            if (len == 5 && memcmp(dat + s, "Queen", 5) == 0) {
                chn_set.push_back((int32_t)(r + 1));
            }
        }
    }

    // ---------------------- BUILD cn_us_set ----------------------
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

    // ---------------------- BUILD title (Shrek 2) candidates ----------------------
    std::vector<int32_t> t_ids;
    {
        GENDB_PHASE("scan_title_driver");
        size_t t_rows = t_title_off.count - 1;
        const int64_t* off = t_title_off.data;
        const char* dat = t_title_dat.data;
        for (size_t r = 0; r < t_rows; ++r) {
            int64_t s = off[r], e = off[r+1];
            size_t len = (size_t)(e - s);
            if (len == 7 && memcmp(dat + s, "Shrek 2", 7) == 0) {
                int32_t year = t_year[r];
                if (year == NULL_INT || year < 2000 || year > 2010) continue;
                t_ids.push_back((int32_t)(r + 1));
            }
        }
    }

    // 3 note literals (Q29a)
    static const char* NOTE_LITS[3] = {
        "(voice)",
        "(voice) (uncredited)",
        "(voice: English version)"
    };
    auto note_matches = [&](const char* p, size_t len) -> bool {
        if (len == 7  && memcmp(p, NOTE_LITS[0], 7)  == 0) return true;
        if (len == 20 && memcmp(p, NOTE_LITS[1], 20) == 0) return true;
        if (len == 24 && memcmp(p, NOTE_LITS[2], 24) == 0) return true;
        return false;
    };

    auto in_chn_set = [&](int32_t v) -> bool {
        for (int32_t x : chn_set) if (x == v) return true;
        return false;
    };

    // ---------------------- MAIN DRIVING SCAN ----------------------
    std::string min_chn, min_name, min_title;
    size_t name_rows = n_name_off.count - 1;
    size_t mk_off_n = mk_off.count;
    size_t cc_off_n = cc_off.count;
    size_t mc_off_n = mc_off.count;
    size_t mi_off_n = mi_off.count;
    size_t ci_off_n = ci_off.count;
    size_t an_off_n = an_off.count;
    size_t pi_off_n = pi_off.count;

    size_t n_after_mk = 0, n_after_cc = 0, n_after_mc = 0, n_after_mi = 0, n_final = 0;

    {
        GENDB_PHASE("main_scan");
        for (int32_t t_id_val : t_ids) {
            int32_t t_row = t_id_val - 1;

            // 1. movie_keyword: keyword_id == cam_kw_id
            if ((size_t)t_id_val + 1 >= mk_off_n) continue;
            int32_t mk_lo = mk_off[t_id_val], mk_hi = mk_off[t_id_val + 1];
            bool mk_ok = false;
            for (int32_t r = mk_lo; r < mk_hi; ++r) {
                if (mk_keyword_id[r] == cam_kw_id) { mk_ok = true; break; }
            }
            if (!mk_ok) continue;
            ++n_after_mk;

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

            // 4. movie_info: it_release && (Japan: or USA:) && contains '200'
            if ((size_t)t_id_val + 1 >= mi_off_n) continue;
            int32_t mi_lo = mi_off[t_id_val], mi_hi = mi_off[t_id_val + 1];
            bool mi_ok = false;
            for (int32_t r = mi_lo; r < mi_hi; ++r) {
                if (mi_info_type_id[r] != it_release_id) continue;
                int64_t s = mi_info_off[r], e = mi_info_off[r+1];
                size_t len = (size_t)(e - s);
                if (len == 0) continue;
                const char* p = mi_info_dat.data + s;
                bool prefix_jp = (len >= 6 && memcmp(p, "Japan:", 6) == 0);
                bool prefix_us = (len >= 4 && memcmp(p, "USA:", 4) == 0);
                if (!prefix_jp && !prefix_us) continue;
                size_t skip = prefix_jp ? 6 : 4;
                if (my_memmem(p + skip, len - skip, "200", 3)) { mi_ok = true; break; }
            }
            if (!mi_ok) continue;
            ++n_after_mi;

            // 5. cast_info: role_id==actress, note ∈ 3-set, person_role_id ∈ chn_set
            if ((size_t)t_id_val + 1 >= ci_off_n) continue;
            int32_t ci_lo = ci_off[t_id_val], ci_hi = ci_off[t_id_val + 1];

            int64_t tts = t_title_off[t_row], tte = t_title_off[t_row + 1];
            const char* t_title_p = t_title_dat.data + tts;
            size_t t_title_len = (size_t)(tte - tts);

            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                if (ci_role_id[r] != rt_actress_id) continue;
                int32_t prid = ci_person_role_id[r];
                if (prid == NULL_INT) continue;
                if (!in_chn_set(prid)) continue;
                int64_t ns = ci_note_off[r], ne = ci_note_off[r+1];
                size_t nlen = (size_t)(ne - ns);
                if (nlen == 0) continue;
                if (!note_matches(ci_note_dat.data + ns, nlen)) continue;

                int32_t p_id = ci_person_id[r];
                if (p_id <= 0 || (size_t)p_id > name_rows) continue;

                // gender == f
                if ((int32_t)n_gender_code[p_id - 1] != f_code) continue;
                // name LIKE '%An%'
                int64_t nns = n_name_off[p_id - 1], nne = n_name_off[p_id];
                size_t nnlen = (size_t)(nne - nns);
                if (!my_memmem(n_name_dat.data + nns, nnlen, "An", 2)) continue;
                // aka_name existence
                if ((size_t)p_id + 1 >= an_off_n) continue;
                if (an_off[p_id + 1] <= an_off[p_id]) continue;
                // person_info: any row with it3_trivia_id
                if ((size_t)p_id + 1 >= pi_off_n) continue;
                int32_t pi_lo = pi_off[p_id], pi_hi = pi_off[p_id + 1];
                bool pi_ok = false;
                for (int32_t pr = pi_lo; pr < pi_hi; ++pr) {
                    if (pi_info_type_id[pr] == it3_trivia_id) { pi_ok = true; break; }
                }
                if (!pi_ok) continue;

                // chn.name (always "Queen" here, but read for correctness)
                int32_t chn_row = prid - 1;
                if (chn_row < 0 || (size_t)chn_row + 1 >= chn_name_off.count) continue;
                int64_t cs = chn_name_off[chn_row], ce = chn_name_off[chn_row + 1];
                const char* chn_p = chn_name_dat.data + cs;
                size_t chn_len = (size_t)(ce - cs);
                if (chn_len == 0) continue;

                min_update(min_chn, chn_p, chn_len);
                min_update(min_name, n_name_dat.data + nns, nnlen);
                min_update(min_title, t_title_p, t_title_len);
                ++n_final;
            }
        }
    }

    fprintf(stderr, "[STATS] t_ids=%zu after_mk=%zu after_cc=%zu after_mc=%zu after_mi=%zu finals=%zu chn_set=%zu\n",
            t_ids.size(), n_after_mk, n_after_cc, n_after_mc, n_after_mi, n_final, chn_set.size());

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
        std::string out_path = results_dir + "/Q29a.csv";
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
