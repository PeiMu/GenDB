// Q27c: MIN(cn.name), MIN(lt.link), MIN(t.title) over sequel-keyword join
// Driver: movie_keyword.keyword_id CSR for 'sequel' (~2800 movies)
// Per t_id: year filter -> ml -> mc -> cc -> mi (all EXISTS), collect ids for MIN materialization

#include "timing_utils.h"
#include "mmap_utils.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <unordered_set>

using namespace gendb;

static inline const char* mymemmem(const char* h, size_t hl, const char* n, size_t nl) {
    if (nl == 0) return h;
    if (hl < nl) return nullptr;
    char first = n[0];
    for (size_t i = 0; i + nl <= hl; ++i) {
        if (h[i] == first && std::memcmp(h + i, n, nl) == 0) return h + i;
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gd = argv[1];
    std::string rd = argv[2];

    // ----- mmap columns -----
    MmapColumn<char>    cct_kind_dat;
    MmapColumn<int64_t> cct_kind_off;
    MmapColumn<int32_t> cct_id;

    MmapColumn<char>    ct_kind_dat;
    MmapColumn<int64_t> ct_kind_off;
    MmapColumn<int32_t> ct_id_col;

    MmapColumn<char>    lt_link_dat;
    MmapColumn<int64_t> lt_link_off;
    MmapColumn<int32_t> lt_id;

    MmapColumn<char>    kw_keyword_dat;
    MmapColumn<int64_t> kw_keyword_off;
    MmapColumn<int32_t> kw_id;

    MmapColumn<int16_t> cn_country_code;
    MmapColumn<char>    cn_cc_dict_dat;
    MmapColumn<int64_t> cn_cc_dict_off;
    MmapColumn<char>    cn_name_dat;
    MmapColumn<int64_t> cn_name_off;
    MmapColumn<int32_t> cn_id;

    MmapColumn<int32_t> title_id;
    MmapColumn<int32_t> title_pyear;
    MmapColumn<char>    title_title_dat;
    MmapColumn<int64_t> title_title_off;

    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mk_kw_off;
    MmapColumn<int32_t> mk_kw_rowids;

    MmapColumn<int32_t> ml_link_type_id;
    MmapColumn<int32_t> ml_mid_off;

    MmapColumn<int32_t> mc_ct_id_col;
    MmapColumn<int32_t> mc_cn_id_col;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<int32_t> mc_mid_off;

    MmapColumn<int32_t> cc_subject_id;
    MmapColumn<int32_t> cc_status_id;
    MmapColumn<int32_t> cc_mid_off;

    MmapColumn<char>    mi_info_dat;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<int32_t> mi_mid_off;

    {
        GENDB_PHASE("data_loading");
        cct_kind_dat.open(gd + "/comp_cast_type/kind.dat");
        cct_kind_off.open(gd + "/comp_cast_type/kind.off");
        cct_id.open(gd + "/comp_cast_type/id.bin");

        ct_kind_dat.open(gd + "/company_type/kind.dat");
        ct_kind_off.open(gd + "/company_type/kind.off");
        ct_id_col.open(gd + "/company_type/id.bin");

        lt_link_dat.open(gd + "/link_type/link.dat");
        lt_link_off.open(gd + "/link_type/link.off");
        lt_id.open(gd + "/link_type/id.bin");

        kw_keyword_dat.open(gd + "/keyword/keyword.dat");
        kw_keyword_off.open(gd + "/keyword/keyword.off");
        kw_id.open(gd + "/keyword/id.bin");

        cn_country_code.open(gd + "/company_name/country_code.bin");
        cn_cc_dict_dat.open(gd + "/company_name/country_code.dict.dat");
        cn_cc_dict_off.open(gd + "/company_name/country_code.dict.off");
        cn_name_dat.open(gd + "/company_name/name.dat");
        cn_name_off.open(gd + "/company_name/name.off");
        cn_id.open(gd + "/company_name/id.bin");

        title_id.open(gd + "/title/id.bin");
        title_pyear.open(gd + "/title/production_year.bin");
        title_title_dat.open(gd + "/title/title.dat");
        title_title_off.open(gd + "/title/title.off");

        mk_movie_id.open(gd + "/movie_keyword/movie_id.bin");
        mk_kw_off.open(gd + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_rowids.open(gd + "/_idx/movie_keyword__keyword_id__rowids.bin");

        ml_link_type_id.open(gd + "/movie_link/link_type_id.bin");
        ml_mid_off.open(gd + "/_idx/movie_link__movie_id__offsets.bin");

        mc_ct_id_col.open(gd + "/movie_companies/company_type_id.bin");
        mc_cn_id_col.open(gd + "/movie_companies/company_id.bin");
        mc_note_off.open(gd + "/movie_companies/note.off");
        mc_mid_off.open(gd + "/_idx/movie_companies__movie_id__offsets.bin");

        cc_subject_id.open(gd + "/complete_cast/subject_id.bin");
        cc_status_id.open(gd + "/complete_cast/status_id.bin");
        cc_mid_off.open(gd + "/_idx/complete_cast__movie_id__offsets.bin");

        mi_info_dat.open(gd + "/movie_info/info.dat");
        mi_info_off.open(gd + "/movie_info/info.off");
        mi_mid_off.open(gd + "/_idx/movie_info__movie_id__offsets.bin");
    }

    // ----- resolve dimensions / build filter sets -----
    int32_t cast_id = -1;
    uint32_t complete_prefix_mask = 0;  // bitmask over cct ids
    int32_t prod_co_ct_id = -1;
    uint32_t lt_follow_mask = 0;        // bitmask over link_type ids (max 18)
    int32_t sequel_kw_id = -1;
    int16_t pl_code = 0;                // 0 means not found (real codes >=1)
    std::vector<uint8_t> cn_pass;       // size max_cn_id+1
    std::unordered_set<std::string> mi_info_set;
    {
        GENDB_PHASE("build_filter_sets");
        // comp_cast_type
        for (size_t i = 0; i < cct_id.size(); ++i) {
            int64_t a = cct_kind_off[i], b = cct_kind_off[i+1];
            const char* s = cct_kind_dat.data + a;
            size_t len = (size_t)(b - a);
            int32_t id = cct_id[i];
            if (len == 4 && std::memcmp(s, "cast", 4) == 0) cast_id = id;
            if (len >= 8 && std::memcmp(s, "complete", 8) == 0) {
                if (id >= 0 && id < 32) complete_prefix_mask |= (1u << id);
            }
        }
        // company_type
        for (size_t i = 0; i < ct_id_col.size(); ++i) {
            int64_t a = ct_kind_off[i], b = ct_kind_off[i+1];
            const char* s = ct_kind_dat.data + a;
            size_t len = (size_t)(b - a);
            if (len == 20 && std::memcmp(s, "production companies", 20) == 0) {
                prod_co_ct_id = ct_id_col[i];
                break;
            }
        }
        // link_type
        for (size_t i = 0; i < lt_id.size(); ++i) {
            int64_t a = lt_link_off[i], b = lt_link_off[i+1];
            const char* s = lt_link_dat.data + a;
            size_t len = (size_t)(b - a);
            if (mymemmem(s, len, "follow", 6)) {
                int32_t id = lt_id[i];
                if (id >= 0 && id < 32) lt_follow_mask |= (1u << id);
            }
        }
        // keyword 'sequel'
        for (size_t i = 0; i < kw_id.size(); ++i) {
            int64_t a = kw_keyword_off[i], b = kw_keyword_off[i+1];
            if ((b - a) == 6 && std::memcmp(kw_keyword_dat.data + a, "sequel", 6) == 0) {
                sequel_kw_id = kw_id[i];
                break;
            }
        }
        // pl_code: dict entry for '[pl]'. Code = dict_index + 1.
        size_t dict_K = cn_cc_dict_off.size() ? cn_cc_dict_off.size() - 1 : 0;
        for (size_t i = 0; i < dict_K; ++i) {
            int64_t a = cn_cc_dict_off[i], b = cn_cc_dict_off[i+1];
            if ((b - a) == 4 && std::memcmp(cn_cc_dict_dat.data + a, "[pl]", 4) == 0) {
                pl_code = (int16_t)(i + 1);
                break;
            }
        }
        // cn_pass bitmap. cn ids are dense 1..N (234997 rows).
        size_t cn_rows = cn_id.size();
        int32_t max_cn_id = 0;
        for (size_t i = 0; i < cn_rows; ++i) {
            if (cn_id[i] > max_cn_id) max_cn_id = cn_id[i];
        }
        cn_pass.assign(max_cn_id + 1, 0);
        for (size_t i = 0; i < cn_rows; ++i) {
            int16_t cc = cn_country_code[i];
            if (cc == pl_code) continue;  // country_code == '[pl]' excluded
            int64_t a = cn_name_off[i], b = cn_name_off[i+1];
            const char* s = cn_name_dat.data + a;
            size_t len = (size_t)(b - a);
            bool has_film = mymemmem(s, len, "Film", 4) != nullptr;
            bool has_warner = false;
            if (!has_film) has_warner = mymemmem(s, len, "Warner", 6) != nullptr;
            if (has_film || has_warner) {
                int32_t id = cn_id[i];
                if (id >= 0 && id <= max_cn_id) cn_pass[id] = 1;
            }
        }
        // mi info literal set
        mi_info_set.insert("Sweden");
        mi_info_set.insert("Norway");
        mi_info_set.insert("Germany");
        mi_info_set.insert("Denmark");
        mi_info_set.insert("Swedish");
        mi_info_set.insert("Denish");
        mi_info_set.insert("Norwegian");
        mi_info_set.insert("German");
        mi_info_set.insert("English");
    }

    if (cast_id < 0 || prod_co_ct_id < 0 || sequel_kw_id < 0 ||
        complete_prefix_mask == 0 || lt_follow_mask == 0) {
        // No matches possible — write empty output.
        std::string out = rd + "/Q27c.csv";
        FILE* f = std::fopen(out.c_str(), "w");
        if (f) {
            std::fprintf(f, "producing_company,link_type,complete_western_sequel\n");
            std::fclose(f);
        }
        return 0;
    }

    // ----- driver loop over sequel movies -----
    // Collect: candidate t_ids, lt_ids (per matching tuple), cn_ids (per matching tuple)
    std::vector<int32_t> match_t_ids;
    std::vector<int32_t> match_lt_ids;
    std::vector<int32_t> match_cn_ids;

    {
        GENDB_PHASE("main_scan");
        int32_t r_lo = mk_kw_off[sequel_kw_id];
        int32_t r_hi = mk_kw_off[sequel_kw_id + 1];

        size_t n_title = title_id.size();
        size_t n_ml_off = ml_mid_off.size();
        size_t n_mc_off = mc_mid_off.size();
        size_t n_cc_off = cc_mid_off.size();
        size_t n_mi_off = mi_mid_off.size();

        for (int32_t r = r_lo; r < r_hi; ++r) {
            int32_t mk_row = mk_kw_rowids[r];
            int32_t t = mk_movie_id[mk_row];
            if (t <= 0) continue;

            // title.production_year filter — title.id is dense 1..N
            size_t t_row = (size_t)(t - 1);
            if (t_row >= n_title) continue;
            int32_t py = title_pyear[t_row];
            if (py < 1950 || py > 2010) continue;

            // ml probe: any link_type_id in lt_follow_mask
            std::vector<int32_t> ml_lts; // capture matching lt_ids
            if ((size_t)(t + 1) >= n_ml_off) continue;
            {
                int32_t lo = ml_mid_off[t];
                int32_t hi = ml_mid_off[t + 1];
                for (int32_t k = lo; k < hi; ++k) {
                    int32_t lid = ml_link_type_id[k];
                    if (lid >= 0 && lid < 32 && ((lt_follow_mask >> lid) & 1u)) {
                        ml_lts.push_back(lid);
                    }
                }
            }
            if (ml_lts.empty()) continue;

            // mc probe: ct_id == prod_co_ct_id AND cn in cn_pass AND note IS NULL
            std::vector<int32_t> mc_cns;
            if ((size_t)(t + 1) >= n_mc_off) continue;
            {
                int32_t lo = mc_mid_off[t];
                int32_t hi = mc_mid_off[t + 1];
                for (int32_t k = lo; k < hi; ++k) {
                    if (mc_ct_id_col[k] != prod_co_ct_id) continue;
                    int32_t cnid = mc_cn_id_col[k];
                    if (cnid < 0 || (size_t)cnid >= cn_pass.size() || !cn_pass[cnid]) continue;
                    // note IS NULL: empty varlen
                    if (mc_note_off[k + 1] - mc_note_off[k] != 0) continue;
                    mc_cns.push_back(cnid);
                }
            }
            if (mc_cns.empty()) continue;

            // cc probe: subject_id == cast_id AND status_id in complete_prefix_mask
            if ((size_t)(t + 1) >= n_cc_off) continue;
            bool cc_ok = false;
            {
                int32_t lo = cc_mid_off[t];
                int32_t hi = cc_mid_off[t + 1];
                for (int32_t k = lo; k < hi; ++k) {
                    if (cc_subject_id[k] != cast_id) continue;
                    int32_t st = cc_status_id[k];
                    if (st >= 0 && st < 32 && ((complete_prefix_mask >> st) & 1u)) {
                        cc_ok = true;
                        break;
                    }
                }
            }
            if (!cc_ok) continue;

            // mi probe: info in mi_info_set (EXISTS)
            if ((size_t)(t + 1) >= n_mi_off) continue;
            bool mi_ok = false;
            {
                int32_t lo = mi_mid_off[t];
                int32_t hi = mi_mid_off[t + 1];
                for (int32_t k = lo; k < hi; ++k) {
                    int64_t a = mi_info_off[k], b = mi_info_off[k + 1];
                    size_t len = (size_t)(b - a);
                    if (len < 5 || len > 9) continue; // quick prefilter
                    std::string s(mi_info_dat.data + a, len);
                    if (mi_info_set.count(s)) { mi_ok = true; break; }
                }
            }
            if (!mi_ok) continue;

            // All EXISTS satisfied: record t_id, and contribute all matching lt_ids and cn_ids.
            match_t_ids.push_back(t);
            for (int32_t l : ml_lts) match_lt_ids.push_back(l);
            for (int32_t c : mc_cns) match_cn_ids.push_back(c);
        }
    }

    // ----- reduce min (lex-min varlen for each column) -----
    std::string min_cn_name, min_lt_link, min_t_title;
    bool have = !match_t_ids.empty();

    {
        GENDB_PHASE("reduce_min");
        if (have) {
            // MIN t.title
            {
                std::sort(match_t_ids.begin(), match_t_ids.end());
                match_t_ids.erase(std::unique(match_t_ids.begin(), match_t_ids.end()), match_t_ids.end());
                const char* best_p = nullptr;
                size_t best_l = 0;
                for (int32_t t : match_t_ids) {
                    size_t row = (size_t)(t - 1);
                    int64_t a = title_title_off[row], b = title_title_off[row + 1];
                    const char* p = title_title_dat.data + a;
                    size_t l = (size_t)(b - a);
                    if (best_p == nullptr) { best_p = p; best_l = l; continue; }
                    size_t cmp_len = best_l < l ? best_l : l;
                    int c = std::memcmp(p, best_p, cmp_len);
                    if (c < 0 || (c == 0 && l < best_l)) { best_p = p; best_l = l; }
                }
                min_t_title.assign(best_p, best_l);
            }
            // MIN lt.link
            {
                std::sort(match_lt_ids.begin(), match_lt_ids.end());
                match_lt_ids.erase(std::unique(match_lt_ids.begin(), match_lt_ids.end()), match_lt_ids.end());
                // Build map from lt id -> row in link_type
                const char* best_p = nullptr;
                size_t best_l = 0;
                for (int32_t lid : match_lt_ids) {
                    // find row in link_type with that id (small table, linear scan)
                    for (size_t i = 0; i < lt_id.size(); ++i) {
                        if (lt_id[i] == lid) {
                            int64_t a = lt_link_off[i], b = lt_link_off[i + 1];
                            const char* p = lt_link_dat.data + a;
                            size_t l = (size_t)(b - a);
                            if (best_p == nullptr) { best_p = p; best_l = l; }
                            else {
                                size_t cmp_len = best_l < l ? best_l : l;
                                int c = std::memcmp(p, best_p, cmp_len);
                                if (c < 0 || (c == 0 && l < best_l)) { best_p = p; best_l = l; }
                            }
                            break;
                        }
                    }
                }
                if (best_p) min_lt_link.assign(best_p, best_l);
            }
            // MIN cn.name (cn id is dense 1..N → row = id - 1)
            {
                std::sort(match_cn_ids.begin(), match_cn_ids.end());
                match_cn_ids.erase(std::unique(match_cn_ids.begin(), match_cn_ids.end()), match_cn_ids.end());
                const char* best_p = nullptr;
                size_t best_l = 0;
                for (int32_t cnid : match_cn_ids) {
                    size_t row = (size_t)(cnid - 1);
                    if (row >= cn_id.size()) continue;
                    int64_t a = cn_name_off[row], b = cn_name_off[row + 1];
                    const char* p = cn_name_dat.data + a;
                    size_t l = (size_t)(b - a);
                    if (best_p == nullptr) { best_p = p; best_l = l; continue; }
                    size_t cmp_len = best_l < l ? best_l : l;
                    int c = std::memcmp(p, best_p, cmp_len);
                    if (c < 0 || (c == 0 && l < best_l)) { best_p = p; best_l = l; }
                }
                if (best_p) min_cn_name.assign(best_p, best_l);
            }
        }
    }

    // ----- output -----
    {
        GENDB_PHASE("output");
        std::string out = rd + "/Q27c.csv";
        FILE* f = std::fopen(out.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open output %s\n", out.c_str()); return 1; }
        std::fprintf(f, "producing_company,link_type,complete_western_sequel\n");
        if (have) {
            std::fwrite(min_cn_name.data(), 1, min_cn_name.size(), f);
            std::fputc(',', f);
            std::fwrite(min_lt_link.data(), 1, min_lt_link.size(), f);
            std::fputc(',', f);
            std::fwrite(min_t_title.data(), 1, min_t_title.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
