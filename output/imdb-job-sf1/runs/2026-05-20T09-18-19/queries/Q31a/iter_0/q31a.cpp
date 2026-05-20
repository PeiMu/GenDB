// Q31a — IMDB JOB
// SELECT MIN(mi.info), MIN(mi_idx.info), MIN(n.name), MIN(t.title)
// FROM ci, cn, it1, it2, k, mc, mi, mi_idx, mk, n, t
// WHERE cn.name LIKE 'Lionsgate%' ...
//
// Strategy (from plan):
//  1. Resolve dims: it1_id='genres', it2_id='votes', m_code='m', k_ids[7],
//     writer-note set, cn_ids (Lionsgate prefix).
//  2. Drive candidate movies from cn_ids via CSR movie_companies__company_id.
//  3. For each candidate movie, probe mk/mi/mi_idx/ci via offsets_only indexes.
//  4. Track 4 running MINs lexicographically.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <climits>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---------- lexicographic compare ----------
static inline int lex_cmp(const char* a, size_t la, const char* b, size_t lb) {
    size_t m = (la < lb) ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

struct MinStr {
    const char* ptr = nullptr;
    size_t len = 0;
    bool has = false;
    void update(const char* p, size_t l) {
        if (!has || lex_cmp(p, l, ptr, len) < 0) {
            ptr = p; len = l; has = true;
        }
    }
};

// ---------- CSV escape ----------
static void csv_write_field(FILE* f, const char* s, size_t n) {
    bool need_quote = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        std::fwrite(s, 1, n, f);
    } else {
        std::fputc('"', f);
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == '"') std::fputc('"', f);
            std::fputc(c, f);
        }
        std::fputc('"', f);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 2;
    }
    std::string sdir = argv[1];
    std::string rdir = argv[2];

    GENDB_PHASE("total");

    // ---------------- data loading ----------------
    MmapColumn<char>    cn_dat, cn_off_raw;
    MmapColumn<char>    it_dat, it_off_raw;
    MmapColumn<int32_t> it_id;
    MmapColumn<char>    kw_dat, kw_off_raw;
    MmapColumn<int32_t> kw_id;
    MmapColumn<char>    gender_dict_dat, gender_dict_off_raw;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<char>    name_dat, name_off_raw;
    MmapColumn<int32_t> mc_movie_id, mc_company_id;
    MmapColumn<int32_t> mc_off_co, mc_row_co;
    MmapColumn<int32_t> mk_movie_id, mk_keyword_id, mk_off_mv;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id, mi_off_mv;
    MmapColumn<char>    mi_info_dat, mi_info_off_raw;
    MmapColumn<int32_t> mix_movie_id, mix_info_type_id, mix_off_mv;
    MmapColumn<char>    mix_info_dat, mix_info_off_raw;
    MmapColumn<int32_t> ci_movie_id, ci_person_id, ci_off_mv;
    MmapColumn<char>    ci_note_dat, ci_note_off_raw;
    MmapColumn<char>    t_title_dat, t_title_off_raw;

    {
        GENDB_PHASE("data_loading");
        cn_dat.open(sdir + "/company_name/name.dat");
        cn_off_raw.open(sdir + "/company_name/name.off");

        it_dat.open(sdir + "/info_type/info.dat");
        it_off_raw.open(sdir + "/info_type/info.off");
        it_id.open(sdir + "/info_type/id.bin");

        kw_dat.open(sdir + "/keyword/keyword.dat");
        kw_off_raw.open(sdir + "/keyword/keyword.off");
        kw_id.open(sdir + "/keyword/id.bin");

        gender_dict_dat.open(sdir + "/name/gender.dict.dat");
        gender_dict_off_raw.open(sdir + "/name/gender.dict.off");
        name_gender.open(sdir + "/name/gender.bin");
        name_dat.open(sdir + "/name/name.dat");
        name_off_raw.open(sdir + "/name/name.off");

        mc_movie_id.open(sdir + "/movie_companies/movie_id.bin");
        mc_company_id.open(sdir + "/movie_companies/company_id.bin");
        mc_off_co.open(sdir + "/_idx/movie_companies__company_id__offsets.bin");
        mc_row_co.open(sdir + "/_idx/movie_companies__company_id__rowids.bin");

        mk_movie_id.open(sdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(sdir + "/movie_keyword/keyword_id.bin");
        mk_off_mv.open(sdir + "/_idx/movie_keyword__movie_id__offsets.bin");

        mi_movie_id.open(sdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(sdir + "/movie_info/info_type_id.bin");
        mi_off_mv.open(sdir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_dat.open(sdir + "/movie_info/info.dat");
        mi_info_off_raw.open(sdir + "/movie_info/info.off");

        mix_movie_id.open(sdir + "/movie_info_idx/movie_id.bin");
        mix_info_type_id.open(sdir + "/movie_info_idx/info_type_id.bin");
        mix_off_mv.open(sdir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mix_info_dat.open(sdir + "/movie_info_idx/info.dat");
        mix_info_off_raw.open(sdir + "/movie_info_idx/info.off");

        ci_movie_id.open(sdir + "/cast_info/movie_id.bin");
        ci_person_id.open(sdir + "/cast_info/person_id.bin");
        ci_off_mv.open(sdir + "/_idx/cast_info__movie_id__offsets.bin");
        ci_note_dat.open(sdir + "/cast_info/note.dat");
        ci_note_off_raw.open(sdir + "/cast_info/note.off");

        t_title_dat.open(sdir + "/title/title.dat");
        t_title_off_raw.open(sdir + "/title/title.off");
    }

    // Reinterpret int64 offset arrays
    const int64_t* cn_off = reinterpret_cast<const int64_t*>(cn_off_raw.data);
    const int64_t* it_off = reinterpret_cast<const int64_t*>(it_off_raw.data);
    const int64_t* kw_off = reinterpret_cast<const int64_t*>(kw_off_raw.data);
    const int64_t* gd_off = reinterpret_cast<const int64_t*>(gender_dict_off_raw.data);
    const int64_t* nm_off = reinterpret_cast<const int64_t*>(name_off_raw.data);
    const int64_t* mi_info_off = reinterpret_cast<const int64_t*>(mi_info_off_raw.data);
    const int64_t* mix_info_off = reinterpret_cast<const int64_t*>(mix_info_off_raw.data);
    const int64_t* ci_note_off = reinterpret_cast<const int64_t*>(ci_note_off_raw.data);
    const int64_t* t_title_off = reinterpret_cast<const int64_t*>(t_title_off_raw.data);

    const size_t n_companies = cn_off_raw.file_size / 8 - 1;
    const size_t n_info_types = it_off_raw.file_size / 8 - 1;
    const size_t n_keywords = kw_off_raw.file_size / 8 - 1;
    const size_t n_genders = gender_dict_off_raw.file_size / 8 - 1;
    const size_t n_names = nm_off ? (name_off_raw.file_size / 8 - 1) : 0;

    // ---------------- resolve dims ----------------
    int32_t it1_id = -1, it2_id = -1;
    {
        GENDB_PHASE("resolve_dims");
        for (size_t i = 0; i < n_info_types; i++) {
            const char* s = it_dat.data + it_off[i];
            size_t len = it_off[i+1] - it_off[i];
            if (len == 6 && std::memcmp(s, "genres", 6) == 0) it1_id = it_id.data[i];
            else if (len == 5 && std::memcmp(s, "votes", 5) == 0) it2_id = it_id.data[i];
        }
        if (it1_id < 0 || it2_id < 0) {
            std::fprintf(stderr, "info_type lookup failed: it1=%d it2=%d\n", it1_id, it2_id);
            return 3;
        }
    }

    // Resolve 'm' code in gender dict.
    // Encoding convention in this dataset: stored byte = dict_index + 1, with 0 = NULL.
    int8_t m_code = -1;
    for (size_t i = 0; i < n_genders; i++) {
        const char* s = gender_dict_dat.data + gd_off[i];
        size_t len = gd_off[i+1] - gd_off[i];
        if (len == 1 && s[0] == 'm') { m_code = (int8_t)(i + 1); break; }
    }
    if (m_code < 0) {
        std::fprintf(stderr, "gender 'm' not found\n");
        return 3;
    }

    // Lionsgate prefix scan → cn_ids
    std::vector<int32_t> cn_ids;
    cn_ids.reserve(64);
    {
        GENDB_PHASE("scan_company_name_prefix");
        const char kPrefix[] = "Lionsgate";
        const size_t kLen = 9;
        for (size_t i = 0; i < n_companies; i++) {
            size_t len = cn_off[i+1] - cn_off[i];
            if (len < kLen) continue;
            if (std::memcmp(cn_dat.data + cn_off[i], kPrefix, kLen) == 0) {
                cn_ids.push_back((int32_t)(i + 1)); // 1-based id
            }
        }
    }

    // Keyword IN-set → k_ids
    static const char* kKw[] = {
        "murder", "violence", "blood", "gore", "death", "female-nudity", "hospital"
    };
    static const size_t kKwLen[] = {6, 8, 5, 4, 5, 13, 8};
    const int kKwN = 7;
    std::vector<int32_t> k_ids_vec;
    k_ids_vec.reserve(kKwN);
    {
        for (size_t i = 0; i < n_keywords; i++) {
            size_t len = kw_off[i+1] - kw_off[i];
            const char* s = kw_dat.data + kw_off[i];
            for (int j = 0; j < kKwN; j++) {
                if (len == kKwLen[j] && std::memcmp(s, kKw[j], len) == 0) {
                    k_ids_vec.push_back(kw_id.data[i]);
                    break;
                }
            }
            if ((int)k_ids_vec.size() == kKwN) break;
        }
    }
    // Small set membership test — linear over up to 7 ids.
    auto k_member = [&](int32_t kid) -> bool {
        for (size_t i = 0; i < k_ids_vec.size(); i++) if (k_ids_vec[i] == kid) return true;
        return false;
    };

    // Writer note set
    static const char* kNotes[] = {
        "(writer)", "(head writer)", "(written by)", "(story)", "(story editor)"
    };
    static const size_t kNoteLen[] = {8, 13, 12, 7, 14};
    auto note_member = [&](const char* p, size_t len) -> bool {
        switch (len) {
            case 8:  return std::memcmp(p, kNotes[0], 8) == 0;
            case 13: return std::memcmp(p, kNotes[1], 13) == 0;
            case 12: return std::memcmp(p, kNotes[2], 12) == 0;
            case 7:  return std::memcmp(p, kNotes[3], 7) == 0;
            case 14: return std::memcmp(p, kNotes[4], 14) == 0;
            default: return false;
        }
    };

    // ---------------- drive candidate movies via mc CSR ----------------
    std::vector<int32_t> candidates;
    candidates.reserve(8192);
    {
        GENDB_PHASE("drive_mc_csr_by_company");
        for (int32_t cid : cn_ids) {
            int32_t lo = mc_off_co.data[cid];
            int32_t hi = mc_off_co.data[cid + 1];
            for (int32_t k = lo; k < hi; k++) {
                int32_t r = mc_row_co.data[k];
                int32_t mv = mc_movie_id.data[r];
                candidates.push_back(mv);
            }
        }
        // Dedupe — movie_companies may have multiple rows per (movie, company)
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    }

    // ---------------- per-movie probe + aggregate ----------------
    MinStr min_mi_info, min_mix_info, min_writer, min_title;

    {
        GENDB_PHASE("main_scan");
        for (int32_t mv : candidates) {
            // (1) movie_keyword semi-join: any mk row with keyword_id in k_ids?
            int32_t mk_lo = mk_off_mv.data[mv];
            int32_t mk_hi = mk_off_mv.data[mv + 1];
            bool has_kw = false;
            for (int32_t r = mk_lo; r < mk_hi; r++) {
                if (k_member(mk_keyword_id.data[r])) { has_kw = true; break; }
            }
            if (!has_kw) continue;

            // (2) movie_info: it1_id AND info in {Horror,Thriller}
            int32_t mi_lo = mi_off_mv.data[mv];
            int32_t mi_hi = mi_off_mv.data[mv + 1];
            // Track best mi.info for this movie (for join-condition + MIN capture)
            const char* best_mi_p = nullptr;
            size_t best_mi_l = 0;
            for (int32_t r = mi_lo; r < mi_hi; r++) {
                if (mi_info_type_id.data[r] != it1_id) continue;
                size_t l = mi_info_off[r + 1] - mi_info_off[r];
                const char* s = mi_info_dat.data + mi_info_off[r];
                bool match = false;
                if (l == 6 && std::memcmp(s, "Horror", 6) == 0) match = true;
                else if (l == 8 && std::memcmp(s, "Thriller", 8) == 0) match = true;
                if (!match) continue;
                if (!best_mi_p || lex_cmp(s, l, best_mi_p, best_mi_l) < 0) {
                    best_mi_p = s; best_mi_l = l;
                }
            }
            if (!best_mi_p) continue;

            // (3) movie_info_idx: it2_id; capture info for MIN
            int32_t mix_lo = mix_off_mv.data[mv];
            int32_t mix_hi = mix_off_mv.data[mv + 1];
            const char* best_mix_p = nullptr;
            size_t best_mix_l = 0;
            for (int32_t r = mix_lo; r < mix_hi; r++) {
                if (mix_info_type_id.data[r] != it2_id) continue;
                size_t l = mix_info_off[r + 1] - mix_info_off[r];
                const char* s = mix_info_dat.data + mix_info_off[r];
                if (!best_mix_p || lex_cmp(s, l, best_mix_p, best_mix_l) < 0) {
                    best_mix_p = s; best_mix_l = l;
                }
            }
            if (!best_mix_p) continue;

            // (4) cast_info: note in writer-set AND gender(person)=='m'; capture name
            int32_t ci_lo = ci_off_mv.data[mv];
            int32_t ci_hi = ci_off_mv.data[mv + 1];
            const char* best_writer_p = nullptr;
            size_t best_writer_l = 0;
            for (int32_t r = ci_lo; r < ci_hi; r++) {
                size_t nl = ci_note_off[r + 1] - ci_note_off[r];
                if (nl == 0) continue;
                const char* np = ci_note_dat.data + ci_note_off[r];
                if (!note_member(np, nl)) continue;
                int32_t pid = ci_person_id.data[r];
                if (pid <= 0 || (size_t)pid > n_names) continue;
                if (name_gender.data[pid - 1] != m_code) continue;
                size_t nml = nm_off[pid] - nm_off[pid - 1];
                const char* nmp = name_dat.data + nm_off[pid - 1];
                if (!best_writer_p || lex_cmp(nmp, nml, best_writer_p, best_writer_l) < 0) {
                    best_writer_p = nmp; best_writer_l = nml;
                }
            }
            if (!best_writer_p) continue;

            // (5) title: identity index, mv -> row mv-1
            size_t tl = t_title_off[mv] - t_title_off[mv - 1];
            const char* tp = t_title_dat.data + t_title_off[mv - 1];

            // All four projections valid → update global MINs.
            min_mi_info.update(best_mi_p, best_mi_l);
            min_mix_info.update(best_mix_p, best_mix_l);
            min_writer.update(best_writer_p, best_writer_l);
            min_title.update(tp, tl);
        }
    }

    // ---------------- output ----------------
    {
        GENDB_PHASE("output");
        mkdir(rdir.c_str(), 0755);
        std::string out_path = rdir + "/Q31a.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::perror("fopen"); return 4; }
        std::fputs("movie_budget,movie_votes,writer,violent_liongate_movie\n", f);

        auto emit = [&](const MinStr& m) {
            if (m.has) csv_write_field(f, m.ptr, m.len);
        };
        emit(min_mi_info);   std::fputc(',', f);
        emit(min_mix_info);  std::fputc(',', f);
        emit(min_writer);    std::fputc(',', f);
        emit(min_title);     std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
