// Q27b: minimum producing_company, link_type, complete_western_sequel
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;
using std::string;
using std::string_view;
using std::vector;

static inline string_view svstr(const uint64_t* off, const char* dat, size_t i) {
    uint64_t lo = off[i], hi = off[i + 1];
    return string_view(dat + lo, hi - lo);
}

static inline bool has_sub(string_view s, const char* needle, size_t n) {
    if (s.size() < n) return false;
    return memmem(s.data(), s.size(), needle, n) != nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "usage: q27b <gendb_dir> <results_dir>\n"); return 1; }
    string gd = argv[1], rd = argv[2];
    std::filesystem::create_directories(rd);

    int32_t year_param = (int32_t)gendb::parse_int_arg(argc, argv, "--year", 1998);

    GENDB_PHASE("total");

    // -------- mmap files --------
    MmapColumn<int32_t> title_id, title_year;
    MmapColumn<uint64_t> title_off; MmapColumn<char> title_dat;

    MmapColumn<int32_t> cct_id;
    MmapColumn<uint64_t> cct_off; MmapColumn<char> cct_dat;

    MmapColumn<int32_t> ct_id;
    MmapColumn<uint64_t> ct_off; MmapColumn<char> ct_dat;

    MmapColumn<int32_t> k_id;
    MmapColumn<uint64_t> k_off; MmapColumn<char> k_dat;

    MmapColumn<int32_t> lt_id;
    MmapColumn<uint64_t> lt_off; MmapColumn<char> lt_dat;

    MmapColumn<int32_t> cn_id;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<uint64_t> cn_name_off; MmapColumn<char> cn_name_dat;
    MmapColumn<uint64_t> cn_dict_off; MmapColumn<char> cn_dict_dat;

    MmapColumn<int32_t> mk_kw_idx_off, mk_kw_rowids;
    MmapColumn<int32_t> mk_movie_id;

    MmapColumn<int32_t> ml_off, ml_link_type_id;
    MmapColumn<int32_t> mc_off, mc_company_id, mc_company_type_id;
    MmapColumn<uint64_t> mc_note_off;
    MmapColumn<int32_t> cc_off, cc_subject_id, cc_status_id;
    MmapColumn<int32_t> mi_off;
    MmapColumn<uint64_t> mi_info_off; MmapColumn<char> mi_info_dat;

    {
        GENDB_PHASE("data_loading");
        title_id.open(gd + "/title/id.bin");
        title_year.open(gd + "/title/production_year.bin");
        title_off.open(gd + "/title/title.off");
        title_dat.open(gd + "/title/title.dat");

        cct_id.open(gd + "/comp_cast_type/id.bin");
        cct_off.open(gd + "/comp_cast_type/kind.off");
        cct_dat.open(gd + "/comp_cast_type/kind.dat");

        ct_id.open(gd + "/company_type/id.bin");
        ct_off.open(gd + "/company_type/kind.off");
        ct_dat.open(gd + "/company_type/kind.dat");

        k_id.open(gd + "/keyword/id.bin");
        k_off.open(gd + "/keyword/keyword.off");
        k_dat.open(gd + "/keyword/keyword.dat");

        lt_id.open(gd + "/link_type/id.bin");
        lt_off.open(gd + "/link_type/link.off");
        lt_dat.open(gd + "/link_type/link.dat");

        cn_id.open(gd + "/company_name/id.bin");
        cn_cc.open(gd + "/company_name/country_code.bin");
        cn_name_off.open(gd + "/company_name/name.off");
        cn_name_dat.open(gd + "/company_name/name.dat");
        cn_dict_off.open(gd + "/company_name/country_code.dict.off");
        cn_dict_dat.open(gd + "/company_name/country_code.dict.dat");

        mk_kw_idx_off.open(gd + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kw_rowids.open(gd + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(gd + "/movie_keyword/movie_id.bin");

        ml_off.open(gd + "/_idx/movie_link__movie_id__offsets.bin");
        ml_link_type_id.open(gd + "/movie_link/link_type_id.bin");

        mc_off.open(gd + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(gd + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gd + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gd + "/movie_companies/note.off");

        cc_off.open(gd + "/_idx/complete_cast__movie_id__offsets.bin");
        cc_subject_id.open(gd + "/complete_cast/subject_id.bin");
        cc_status_id.open(gd + "/complete_cast/status_id.bin");

        mi_off.open(gd + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off.open(gd + "/movie_info/info.off");
        mi_info_dat.open(gd + "/movie_info/info.dat");
    }

    // -------- resolve dims --------
    int32_t cct_cast = -1, cct_crew = -1, complete_id = -1;
    for (size_t i = 0; i < cct_id.count; i++) {
        auto s = svstr(cct_off.data, cct_dat.data, i);
        if (s == "cast") cct_cast = cct_id[i];
        else if (s == "crew") cct_crew = cct_id[i];
        else if (s == "complete") complete_id = cct_id[i];
    }
    int32_t ct_pc = -1;
    for (size_t i = 0; i < ct_id.count; i++) {
        auto s = svstr(ct_off.data, ct_dat.data, i);
        if (s == "production companies") { ct_pc = ct_id[i]; break; }
    }
    int32_t sequel_kw_id = -1;
    for (size_t i = 0; i < k_id.count; i++) {
        auto s = svstr(k_off.data, k_dat.data, i);
        if (s == "sequel") { sequel_kw_id = k_id[i]; break; }
    }

    // link_type: build by id (small, ids 1..18 typically)
    int32_t lt_max_id = 0;
    for (size_t r = 0; r < lt_id.count; r++) if (lt_id[r] > lt_max_id) lt_max_id = lt_id[r];
    vector<int32_t> lt_row_by_id(lt_max_id + 2, -1);
    vector<uint8_t> lt_inset(lt_max_id + 2, 0);
    for (size_t r = 0; r < lt_id.count; r++) {
        int32_t id = lt_id[r];
        lt_row_by_id[id] = (int32_t)r;
        auto s = svstr(lt_off.data, lt_dat.data, r);
        if (has_sub(s, "follow", 6)) lt_inset[id] = 1;
    }

    // pl_code from dict (dict_off has N+1 entries; code i corresponds to dict entry i; code 0 = NULL)
    int16_t pl_code = -1;
    size_t dict_n = (cn_dict_off.count > 0) ? (cn_dict_off.count - 1) : 0;
    for (size_t i = 0; i < dict_n; i++) {
        auto s = svstr(cn_dict_off.data, cn_dict_dat.data, i);
        if (s == "[pl]") { pl_code = (int16_t)i; break; }
    }
    // The dict-encoded code in country_code.bin: typically code 0 reserved for NULL, codes 1..N for entries.
    // If first dict entry [de] is code 1, then pl_code = pl_index+1. Let's verify: try both. We'll auto-detect by checking actual cn_cc values for the "[pl]" string by sampling.
    // Simpler: scan unique cc values and find which maps to "[pl]". We'll build a mapping: for each row, cc -> dict index.
    // The standard GenDB convention: dict code maps directly to dict entry index (0..N-1), NULL handled separately.
    // We'll use both possible encodings safely. Given the guide: "dict code 0 = NULL; codes start at 1 if 1-indexed".
    // Use the convention: code value maps to dict[code-1] (1-indexed). Adjust pl_code = pl_index + 1.
    int16_t pl_code_1based = (pl_code >= 0) ? (int16_t)(pl_code + 1) : -1;

    // company_name max id
    int32_t cn_max_id = 0;
    for (size_t r = 0; r < cn_id.count; r++) if (cn_id[r] > cn_max_id) cn_max_id = cn_id[r];

    // cn bitset + row_by_id for matched companies
    vector<uint64_t> cn_set(((size_t)cn_max_id + 64) / 64 + 1, 0);
    vector<int32_t> cn_row_by_id((size_t)cn_max_id + 2, -1);
    for (size_t r = 0; r < cn_id.count; r++) {
        int32_t id = cn_id[r];
        cn_row_by_id[id] = (int32_t)r;
        int16_t cc = cn_cc[r];
        if (cc == pl_code_1based) continue;
        auto s = svstr(cn_name_off.data, cn_name_dat.data, r);
        bool hit = has_sub(s, "Film", 4) || has_sub(s, "Warner", 6);
        if (hit) cn_set[(uint32_t)id / 64] |= (1ULL << ((uint32_t)id % 64));
    }
    auto cn_in = [&](int32_t id) -> bool {
        if (id < 0 || id > cn_max_id) return false;
        return (cn_set[(uint32_t)id / 64] >> ((uint32_t)id % 64)) & 1ULL;
    };

    // title id -> row/year
    int32_t t_max_id = 0;
    for (size_t r = 0; r < title_id.count; r++) if (title_id[r] > t_max_id) t_max_id = title_id[r];
    vector<int32_t> year_by_id((size_t)t_max_id + 2, 0);
    vector<int32_t> trow_by_id((size_t)t_max_id + 2, -1);
    for (size_t r = 0; r < title_id.count; r++) {
        int32_t id = title_id[r];
        year_by_id[id] = title_year[r];
        trow_by_id[id] = (int32_t)r;
    }

    // mi info set
    static const char* MI_LITS[4] = {"Sweden","Germany","Swedish","German"};
    static const size_t MI_LENS[4] = {6,7,7,6};
    auto mi_match = [&](string_view s) {
        size_t n = s.size();
        for (int i = 0; i < 4; i++) {
            if (n == MI_LENS[i] && memcmp(s.data(), MI_LITS[i], n) == 0) return true;
        }
        return false;
    };

    // -------- main scan --------
    string_view min_cn_name, min_lt_link, min_title;
    bool any = false;

    {
        GENDB_PHASE("main_scan");
        if (sequel_kw_id < 0) {
            // nothing
        } else {
            int32_t lo_k = mk_kw_idx_off[sequel_kw_id];
            int32_t hi_k = mk_kw_idx_off[sequel_kw_id + 1];
            size_t ml_max = ml_off.count;
            size_t mc_max = mc_off.count;
            size_t cc_max = cc_off.count;
            size_t mi_max = mi_off.count;
            for (int32_t k = lo_k; k < hi_k; k++) {
                int32_t mk_row = mk_kw_rowids[k];
                int32_t t_id = mk_movie_id[mk_row];
                if ((size_t)t_id >= year_by_id.size()) continue;
                if (year_by_id[t_id] != year_param) continue;

                // ml probe: at least one row with lt in set; collect min lt link
                string_view best_lt;
                bool ml_hit = false;
                if ((size_t)(t_id + 1) < ml_max) {
                    int32_t lo = ml_off[t_id], hi = ml_off[t_id + 1];
                    for (int32_t r = lo; r < hi; r++) {
                        int32_t lt = ml_link_type_id[r];
                        if (lt >= 0 && lt <= lt_max_id && lt_inset[lt]) {
                            ml_hit = true;
                            auto link_s = svstr(lt_off.data, lt_dat.data, lt_row_by_id[lt]);
                            if (best_lt.empty() && best_lt.data() == nullptr) best_lt = link_s;
                            else if (link_s < best_lt) best_lt = link_s;
                        }
                    }
                }
                if (!ml_hit) continue;

                // mc probe
                string_view best_cn;
                bool mc_hit = false;
                if ((size_t)(t_id + 1) < mc_max) {
                    int32_t lo = mc_off[t_id], hi = mc_off[t_id + 1];
                    for (int32_t r = lo; r < hi; r++) {
                        if (mc_company_type_id[r] != ct_pc) continue;
                        if (mc_note_off[r + 1] != mc_note_off[r]) continue; // note must be NULL
                        int32_t cid = mc_company_id[r];
                        if (!cn_in(cid)) continue;
                        mc_hit = true;
                        int32_t cn_r = cn_row_by_id[cid];
                        auto nm = svstr(cn_name_off.data, cn_name_dat.data, cn_r);
                        if (best_cn.data() == nullptr) best_cn = nm;
                        else if (nm < best_cn) best_cn = nm;
                    }
                }
                if (!mc_hit) continue;

                // cc probe
                bool cc_hit = false;
                if ((size_t)(t_id + 1) < cc_max) {
                    int32_t lo = cc_off[t_id], hi = cc_off[t_id + 1];
                    for (int32_t r = lo; r < hi; r++) {
                        int32_t subj = cc_subject_id[r];
                        if (subj != cct_cast && subj != cct_crew) continue;
                        if (cc_status_id[r] != complete_id) continue;
                        cc_hit = true; break;
                    }
                }
                if (!cc_hit) continue;

                // mi probe
                bool mi_hit = false;
                if ((size_t)(t_id + 1) < mi_max) {
                    int32_t lo = mi_off[t_id], hi = mi_off[t_id + 1];
                    for (int32_t r = lo; r < hi; r++) {
                        auto s = svstr(mi_info_off.data, mi_info_dat.data, r);
                        if (mi_match(s)) { mi_hit = true; break; }
                    }
                }
                if (!mi_hit) continue;

                // Surviving tuple: update mins
                auto tit = svstr(title_off.data, title_dat.data, trow_by_id[t_id]);
                if (!any) {
                    min_cn_name = best_cn;
                    min_lt_link = best_lt;
                    min_title = tit;
                    any = true;
                } else {
                    if (best_cn < min_cn_name) min_cn_name = best_cn;
                    if (best_lt < min_lt_link) min_lt_link = best_lt;
                    if (tit < min_title) min_title = tit;
                }
            }
        }
    }

    // -------- write CSV --------
    {
        GENDB_PHASE("output");
        string out_path = rd + "/Q27b.csv";
        FILE* f = fopen(out_path.c_str(), "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 1; }
        fprintf(f, "producing_company,link_type,complete_western_sequel\n");
        if (any) {
            fwrite(min_cn_name.data(), 1, min_cn_name.size(), f);
            fputc(',', f);
            fwrite(min_lt_link.data(), 1, min_lt_link.size(), f);
            fputc(',', f);
            fwrite(min_title.data(), 1, min_title.size(), f);
            fputc('\n', f);
        }
        fclose(f);
    }
    return 0;
}
