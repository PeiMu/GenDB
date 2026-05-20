// Q21a — IMDB JOB
// Find min(cn.name), min(lt.link), min(t.title) over follow-up sequels with
// production companies in Nordic / German countries, prod year 1950..2000.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

static inline bool contains_pat(const char* hay, size_t n, const char* needle, size_t m) {
    if (n < m) return false;
    return memmem(hay, n, needle, m) != nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string out_dir = argv[2];
    std::filesystem::create_directories(out_dir);

    GENDB_PHASE("total");

    // ----- Result aggregators (global lex-min strings) -----
    std::string min_cn_name; bool have_cn = false;
    std::string min_lt_link; bool have_lt = false;
    std::string min_title;   bool have_title = false;

    // ============================================================
    // 1. Data loading + dim resolution
    // ============================================================
    MmapColumn<char>     lt_dat(store + "/link_type/link.dat");
    MmapColumn<int64_t>  lt_off(store + "/link_type/link.off");

    MmapColumn<char>     kw_dat(store + "/keyword/keyword.dat");
    MmapColumn<int64_t>  kw_off(store + "/keyword/keyword.off");

    MmapColumn<char>     ct_dat(store + "/company_type/kind.dat");
    MmapColumn<int64_t>  ct_off(store + "/company_type/kind.off");

    MmapColumn<char>     cn_dict_dat(store + "/company_name/country_code.dict.dat");
    MmapColumn<int64_t>  cn_dict_off(store + "/company_name/country_code.dict.off");
    MmapColumn<int16_t>  cn_cc(store + "/company_name/country_code.bin");

    MmapColumn<char>     cn_name_dat(store + "/company_name/name.dat");
    MmapColumn<int64_t>  cn_name_off(store + "/company_name/name.off");

    MmapColumn<int32_t>  ml_mid(store + "/movie_link/movie_id.bin");
    MmapColumn<int32_t>  ml_lt(store + "/movie_link/link_type_id.bin");

    MmapColumn<int32_t>  mll_off(store + "/_idx/movie_link__link_type_id__offsets.bin");
    MmapColumn<int32_t>  mll_row(store + "/_idx/movie_link__link_type_id__rowids.bin");

    MmapColumn<int32_t>  mk_mid(store + "/movie_keyword/movie_id.bin");
    MmapColumn<int32_t>  mk_kw(store + "/movie_keyword/keyword_id.bin");
    MmapColumn<int32_t>  mkk_off(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
    MmapColumn<int32_t>  mkk_row(store + "/_idx/movie_keyword__keyword_id__rowids.bin");

    MmapColumn<int32_t>  t_py(store + "/title/production_year.bin");
    MmapColumn<char>     t_title_dat(store + "/title/title.dat");
    MmapColumn<int64_t>  t_title_off(store + "/title/title.off");

    MmapColumn<int32_t>  mi_mid(store + "/movie_info/movie_id.bin");
    MmapColumn<char>     mi_info_dat(store + "/movie_info/info.dat");
    MmapColumn<int64_t>  mi_info_off(store + "/movie_info/info.off");
    MmapColumn<int32_t>  mi_mvoff(store + "/_idx/movie_info__movie_id__offsets.bin");

    MmapColumn<int32_t>  mc_cid(store + "/movie_companies/company_id.bin");
    MmapColumn<int32_t>  mc_ctid(store + "/movie_companies/company_type_id.bin");
    MmapColumn<int64_t>  mc_note_off(store + "/movie_companies/note.off");
    MmapColumn<int32_t>  mc_mvoff(store + "/_idx/movie_companies__movie_id__offsets.bin");

    int16_t cc_pl = -1;
    int32_t ct_pc = -1;
    int32_t k_sequel = -1;
    std::vector<int32_t> LT_ids; LT_ids.reserve(8);

    {
        GENDB_PHASE("resolve_dims");

        // ct_pc: scan company_type for 'production companies'
        const char* PC = "production companies";
        size_t PC_n = std::strlen(PC);
        size_t ct_rows = ct_off.count - 1;
        for (size_t i = 0; i < ct_rows; ++i) {
            size_t lo = ct_off[i], hi = ct_off[i+1];
            if ((size_t)(hi - lo) == PC_n && std::memcmp(ct_dat.data + lo, PC, PC_n) == 0) {
                ct_pc = (int32_t)(i + 1); // dense-PK id
                break;
            }
        }

        // k_sequel: scan keyword for 'sequel'
        const char* SEQ = "sequel";
        size_t SEQ_n = 6;
        size_t kw_rows = kw_off.count - 1;
        for (size_t i = 0; i < kw_rows; ++i) {
            size_t lo = kw_off[i], hi = kw_off[i+1];
            if ((size_t)(hi - lo) == SEQ_n && std::memcmp(kw_dat.data + lo, SEQ, SEQ_n) == 0) {
                k_sequel = (int32_t)(i + 1);
                break;
            }
        }

        // cc_pl: scan country_code dict for '[pl]'
        const char* PL = "[pl]";
        size_t PL_n = 4;
        size_t dict_n = cn_dict_off.count - 1;
        for (size_t i = 0; i < dict_n; ++i) {
            size_t lo = cn_dict_off[i], hi = cn_dict_off[i+1];
            if ((size_t)(hi - lo) == PL_n && std::memcmp(cn_dict_dat.data + lo, PL, PL_n) == 0) {
                cc_pl = (int16_t)i;
                break;
            }
        }

        // LT_ids: scan link_type for link LIKE '%follow%'
        size_t lt_rows = lt_off.count - 1;
        const char* FOLLOW = "follow";
        size_t FOLLOW_n = 6;
        for (size_t i = 0; i < lt_rows; ++i) {
            size_t lo = lt_off[i], hi = lt_off[i+1];
            if (contains_pat(lt_dat.data + lo, hi - lo, FOLLOW, FOLLOW_n)) {
                LT_ids.push_back((int32_t)(i + 1));
            }
        }

        std::fprintf(stderr, "[Q21a] ct_pc=%d k_sequel=%d cc_pl=%d |LT|=%zu\n",
                     ct_pc, k_sequel, (int)cc_pl, LT_ids.size());
        if (ct_pc < 0 || k_sequel < 0 || cc_pl < 0 || LT_ids.empty()) {
            std::fprintf(stderr, "[Q21a] dim resolution failed\n");
        }
    }

    // ============================================================
    // 2. Build CN_ids bitset
    // ============================================================
    size_t cn_rows = cn_cc.count; // 234997
    std::vector<uint8_t> CN_ids(cn_rows, 0);
    {
        GENDB_PHASE("build_cn_bitset");
        for (size_t i = 0; i < cn_rows; ++i) {
            int16_t cc = cn_cc[i];
            if (cc == 0) continue;        // NULL excluded (mimic <> semantics)
            if (cc == cc_pl) continue;
            size_t lo = cn_name_off[i], hi = cn_name_off[i+1];
            size_t n = hi - lo;
            const char* s = cn_name_dat.data + lo;
            if (contains_pat(s, n, "Film", 4) || contains_pat(s, n, "Warner", 6)) {
                CN_ids[i] = 1;
            }
        }
    }

    // ============================================================
    // 3. Build sequel bitset over movie_id
    // ============================================================
    // title has 2528312 rows, max movie_id = 2528312
    size_t MAX_MV = t_py.count; // 2528312
    std::vector<uint8_t> seq_bitset(MAX_MV + 2, 0);
    {
        GENDB_PHASE("build_seq_bitset");
        int32_t lo = mkk_off[k_sequel];
        int32_t hi = mkk_off[k_sequel + 1];
        for (int32_t r = lo; r < hi; ++r) {
            int32_t rowid = mkk_row[r];
            int32_t mv = mk_mid[rowid];
            if (mv >= 0 && (size_t)mv <= MAX_MV) seq_bitset[mv] = 1;
        }
    }

    // ============================================================
    // 4. Drive movie_link by LT_ids → collect (mv, lt_id) candidates
    // ============================================================
    struct Cand { int32_t mv; int32_t lt; };
    std::vector<Cand> cands;
    cands.reserve(16384);
    {
        GENDB_PHASE("drive_ml");
        for (int32_t lt : LT_ids) {
            int32_t lo = mll_off[lt];
            int32_t hi = mll_off[lt + 1];
            for (int32_t r = lo; r < hi; ++r) {
                int32_t rowid = mll_row[r];
                int32_t mv = ml_mid[rowid];
                cands.push_back({mv, lt});
            }
        }
    }

    // ============================================================
    // 5. Filter by title.production_year and seq_bitset
    // ============================================================
    {
        GENDB_PHASE("filter_year_sequel");
        size_t w = 0;
        for (size_t i = 0; i < cands.size(); ++i) {
            int32_t mv = cands[i].mv;
            if (mv <= 0 || (size_t)mv > MAX_MV) continue;
            int32_t py = t_py[mv - 1];
            if (py == INT32_MIN || py < 1950 || py > 2000) continue;
            if (!seq_bitset[mv]) continue;
            cands[w++] = cands[i];
        }
        cands.resize(w);
    }

    // ============================================================
    // 6. Main scan: probe mi + mc per candidate mv
    // ============================================================
    // MI_set: 8 country literals
    static const char* MI_LITS[8] = {
        "Sweden", "Norway", "Germany", "Denmark",
        "Swedish", "Denish", "Norwegian", "German"
    };
    static const size_t MI_LENS[8] = {6, 6, 7, 7, 7, 6, 9, 6};

    {
        GENDB_PHASE("main_scan");
        for (size_t i = 0; i < cands.size(); ++i) {
            int32_t mv = cands[i].mv;
            int32_t lt = cands[i].lt;

            // ---- movie_info range probe (early-exit) ----
            int32_t mi_lo = mi_mvoff[mv];
            int32_t mi_hi = mi_mvoff[mv + 1];
            bool mi_hit = false;
            for (int32_t r = mi_lo; r < mi_hi && !mi_hit; ++r) {
                size_t off_lo = mi_info_off[r];
                size_t off_hi = mi_info_off[r + 1];
                size_t n = off_hi - off_lo;
                const char* s = mi_info_dat.data + off_lo;
                for (int k = 0; k < 8; ++k) {
                    if (n != MI_LENS[k]) continue;
                    if (std::memcmp(s, MI_LITS[k], n) == 0) { mi_hit = true; break; }
                }
            }
            if (!mi_hit) continue;

            // ---- movie_companies range probe ----
            int32_t mc_lo = mc_mvoff[mv];
            int32_t mc_hi = mc_mvoff[mv + 1];
            for (int32_t r = mc_lo; r < mc_hi; ++r) {
                // note IS NULL via offsets equality
                if (mc_note_off[r] != mc_note_off[r + 1]) continue;
                if (mc_ctid[r] != ct_pc) continue;
                int32_t cid = mc_cid[r];
                if (cid <= 0 || (size_t)cid > cn_rows) continue;
                if (!CN_ids[cid - 1]) continue;

                // Match. Update aggregates.
                // company name
                {
                    size_t lo = cn_name_off[cid - 1];
                    size_t hi = cn_name_off[cid];
                    std::string_view sv(cn_name_dat.data + lo, hi - lo);
                    if (!have_cn || sv < std::string_view(min_cn_name)) {
                        min_cn_name.assign(sv);
                        have_cn = true;
                    }
                }
                // link type
                {
                    size_t lo = lt_off[lt - 1];
                    size_t hi = lt_off[lt];
                    std::string_view sv(lt_dat.data + lo, hi - lo);
                    if (!have_lt || sv < std::string_view(min_lt_link)) {
                        min_lt_link.assign(sv);
                        have_lt = true;
                    }
                }
                // title
                {
                    size_t lo = t_title_off[mv - 1];
                    size_t hi = t_title_off[mv];
                    std::string_view sv(t_title_dat.data + lo, hi - lo);
                    if (!have_title || sv < std::string_view(min_title)) {
                        min_title.assign(sv);
                        have_title = true;
                    }
                }
            }
        }
    }

    // ============================================================
    // 7. Output
    // ============================================================
    {
        GENDB_PHASE("output");
        std::string out_path = out_dir + "/Q21a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(fp, "company_name,link_type,western_follow_up\n");
        if (have_cn || have_lt || have_title) {
            std::fprintf(fp, "%s,%s,%s\n",
                         min_cn_name.c_str(),
                         min_lt_link.c_str(),
                         min_title.c_str());
        }
        std::fclose(fp);
    }

    return 0;
}
