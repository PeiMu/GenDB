// Q11b: IMDB Join Order Benchmark
// MIN(cn.name), MIN(lt.link), MIN(t.title) — title-driven plan
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <omp.h>

#define _GNU_SOURCE
#include <string.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static inline bool contains(const char* hay, size_t hl, const char* needle, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    return memmem(hay, hl, needle, nl) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: q11b <gendb_dir> <results_dir>\n");
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];

    // Make output dir
    mkdir(rdir.c_str(), 0755);

    // Parameters (defaults inferred from template)
    int32_t param_year = (int32_t)parse_int_arg(argc, argv, "--year", 1998);
    std::string param_title_sub = parse_string_arg(argc, argv, "--title_sub", "Money");
    std::string param_country = parse_string_arg(argc, argv, "--country", "[pl]");
    std::string param_kind = parse_string_arg(argc, argv, "--kind", "production companies");
    std::string param_keyword = parse_string_arg(argc, argv, "--keyword", "sequel");
    std::string param_link_sub = parse_string_arg(argc, argv, "--link_sub", "follows");
    std::string param_name_sub1 = parse_string_arg(argc, argv, "--name_sub1", "Film");
    std::string param_name_sub2 = parse_string_arg(argc, argv, "--name_sub2", "Warner");

    // -----------------------------------------------------------------------
    // Phase: data_loading — mmap columns and indexes
    // -----------------------------------------------------------------------
    MmapColumn<int32_t> t_pyear, t_id;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<int16_t>  cn_cc;
    MmapColumn<uint64_t> cn_dict_off;
    MmapColumn<char>     cn_dict_dat;
    MmapColumn<uint64_t> cn_name_off;
    MmapColumn<char>     cn_name_dat;
    MmapColumn<int32_t>  cn_id;

    MmapColumn<uint64_t> ct_kind_off;
    MmapColumn<char>     ct_kind_dat;
    MmapColumn<int32_t>  ct_id;

    MmapColumn<uint64_t> k_kw_off;
    MmapColumn<char>     k_kw_dat;
    MmapColumn<int32_t>  k_id_col;

    MmapColumn<uint64_t> lt_link_off;
    MmapColumn<char>     lt_link_dat;
    MmapColumn<int32_t>  lt_id_col;

    MmapColumn<int32_t> ml_movie_id, ml_link_type_id;
    MmapColumn<int32_t> mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t> mc_movie_id, mc_company_id, mc_company_type_id;
    MmapColumn<uint64_t> mc_note_off;

    MmapColumn<int32_t> ml_off, mk_off, mc_off;

    {
        GENDB_PHASE("data_loading");
        t_pyear.open(gdir + "/title/production_year.bin");
        t_id.open(gdir + "/title/id.bin");
        t_title_off.open(gdir + "/title/title.off");
        t_title_dat.open(gdir + "/title/title.dat");

        cn_cc.open(gdir + "/company_name/country_code.bin");
        cn_dict_off.open(gdir + "/company_name/country_code.dict.off");
        cn_dict_dat.open(gdir + "/company_name/country_code.dict.dat");
        cn_name_off.open(gdir + "/company_name/name.off");
        cn_name_dat.open(gdir + "/company_name/name.dat");
        cn_id.open(gdir + "/company_name/id.bin");

        ct_kind_off.open(gdir + "/company_type/kind.off");
        ct_kind_dat.open(gdir + "/company_type/kind.dat");
        ct_id.open(gdir + "/company_type/id.bin");

        k_kw_off.open(gdir + "/keyword/keyword.off");
        k_kw_dat.open(gdir + "/keyword/keyword.dat");
        k_id_col.open(gdir + "/keyword/id.bin");

        lt_link_off.open(gdir + "/link_type/link.off");
        lt_link_dat.open(gdir + "/link_type/link.dat");
        lt_id_col.open(gdir + "/link_type/id.bin");

        ml_movie_id.open(gdir + "/movie_link/movie_id.bin");
        ml_link_type_id.open(gdir + "/movie_link/link_type_id.bin");

        mk_movie_id.open(gdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");

        mc_movie_id.open(gdir + "/movie_companies/movie_id.bin");
        mc_company_id.open(gdir + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gdir + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gdir + "/movie_companies/note.off");

        ml_off.open(gdir + "/_idx/movie_link__movie_id__offsets.bin");
        mk_off.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mc_off.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");

        ml_movie_id.advise_random(); // probe access
        mc_company_id.advise_random();
        mc_company_type_id.advise_random();
        mc_note_off.advise_random();
    }

    // -----------------------------------------------------------------------
    // Phase: dim_resolution
    // -----------------------------------------------------------------------
    int16_t pl_code = -1;
    {
        GENDB_PHASE("dim_resolve");
        // country_code dict: code i ∈ [0, n-1]; off[i]..off[i+1] is byte range
        size_t n_codes = cn_dict_off.count - 1;
        for (size_t i = 0; i < n_codes; i++) {
            uint64_t a = cn_dict_off[i], b = cn_dict_off[i + 1];
            size_t len = (size_t)(b - a);
            if (len == param_country.size() &&
                memcmp(cn_dict_dat.data + a, param_country.data(), len) == 0) {
                pl_code = (int16_t)i;
                break;
            }
        }

        // company_type.kind == "production companies"
        int32_t ct_target = -1;
        for (size_t i = 0; i < ct_id.count; i++) {
            uint64_t a = ct_kind_off[i], b = ct_kind_off[i + 1];
            size_t len = (size_t)(b - a);
            if (len == param_kind.size() &&
                memcmp(ct_kind_dat.data + a, param_kind.data(), len) == 0) {
                ct_target = ct_id[i];
                break;
            }
        }

        // keyword == "sequel"
        int32_t k_target = -1;
        for (size_t i = 0; i < k_id_col.count; i++) {
            uint64_t a = k_kw_off[i], b = k_kw_off[i + 1];
            size_t len = (size_t)(b - a);
            if (len == param_keyword.size() &&
                memcmp(k_kw_dat.data + a, param_keyword.data(), len) == 0) {
                k_target = k_id_col[i];
                break;
            }
        }

        // link_type: link contains "follows" → set of (link_id → link string)
        std::vector<int32_t> LT_ids;
        std::vector<std::string_view> LT_strings_by_id; // indexed by lt_id (we'll store by id)
        // Find max lt id
        int32_t max_lt_id = 0;
        for (size_t i = 0; i < lt_id_col.count; i++) {
            if (lt_id_col[i] > max_lt_id) max_lt_id = lt_id_col[i];
        }
        LT_strings_by_id.assign((size_t)max_lt_id + 1, std::string_view());
        for (size_t i = 0; i < lt_id_col.count; i++) {
            uint64_t a = lt_link_off[i], b = lt_link_off[i + 1];
            size_t len = (size_t)(b - a);
            if (contains(lt_link_dat.data + a, len, param_link_sub.data(), param_link_sub.size())) {
                int32_t lid = lt_id_col[i];
                LT_ids.push_back(lid);
                LT_strings_by_id[lid] = std::string_view(lt_link_dat.data + a, len);
            }
        }

        // Stash dim results into module-level scope via static (use lambdas? simpler: continue in main)
        // We'll just inline below.

        if (pl_code < 0 || ct_target < 0 || k_target < 0 || LT_ids.empty()) {
            // Nothing to do
            std::fprintf(stderr, "dim resolution failed: pl=%d ct=%d k=%d LT=%zu\n",
                         (int)pl_code, (int)ct_target, (int)k_target, LT_ids.size());
            // Emit empty result
            std::string out = rdir + "/Q11b.csv";
            FILE* f = std::fopen(out.c_str(), "w");
            std::fprintf(f, "from_company,movie_link_type,sequel_movie\n");
            std::fclose(f);
            return 0;
        }

        // Build small LT bitmap (max_lt_id+1 bytes) for O(1) membership
        std::vector<uint8_t> LT_mask((size_t)max_lt_id + 1, 0);
        for (int32_t l : LT_ids) LT_mask[l] = 1;

        // ---------------------------------------------------------------
        // Phase: main_scan — parallel scan over title, probes inline
        // ---------------------------------------------------------------
        // Thread-local best (smallest) string_views for each MIN
        int nthreads = omp_get_max_threads();
        struct Local {
            std::string best_cn;
            std::string best_lt;
            std::string best_t;
            bool has_cn = false, has_lt = false, has_t = false;
        };
        std::vector<Local> locals((size_t)nthreads);

        const int32_t* py = t_pyear.data;
        const uint64_t* toff = t_title_off.data;
        const char* tdat = t_title_dat.data;
        const int32_t* ml_off_p = ml_off.data;
        const int32_t* mk_off_p = mk_off.data;
        const int32_t* mc_off_p = mc_off.data;
        const int32_t* ml_lt = ml_link_type_id.data;
        const int32_t* mk_kid = mk_keyword_id.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int32_t* mc_ctid = mc_company_type_id.data;
        const uint64_t* mc_noff = mc_note_off.data;
        const int16_t* cn_cc_p = cn_cc.data;
        const uint64_t* cn_noff_p = cn_name_off.data;
        const char* cn_ndat_p = cn_name_dat.data;

        size_t n_titles = t_id.count;
        const char* tsub = param_title_sub.data();
        size_t tsub_len = param_title_sub.size();
        const char* nsub1 = param_name_sub1.data();
        size_t nsub1_len = param_name_sub1.size();
        const char* nsub2 = param_name_sub2.data();
        size_t nsub2_len = param_name_sub2.size();

        {
            GENDB_PHASE("main_scan");
            #pragma omp parallel for schedule(dynamic, 65536)
            for (size_t i = 0; i < n_titles; i++) {
                if (py[i] != param_year) continue;
                uint64_t a = toff[i], b = toff[i + 1];
                size_t tlen = (size_t)(b - a);
                if (tlen < tsub_len) continue;
                if (!memmem(tdat + a, tlen, tsub, tsub_len)) continue;

                int32_t mid = (int32_t)(i + 1); // title id is dense 1-based

                // ----- Probe movie_link: any row with link_type_id ∈ LT -----
                int32_t ml_lo = ml_off_p[mid];
                int32_t ml_hi = ml_off_p[mid + 1];
                std::string_view ml_link_sv;
                bool ml_ok = false;
                for (int32_t r = ml_lo; r < ml_hi; r++) {
                    int32_t ltid = ml_lt[r];
                    if (ltid >= 0 && (size_t)ltid < LT_strings_by_id.size() && LT_mask[ltid]) {
                        if (!ml_ok) {
                            ml_link_sv = LT_strings_by_id[ltid];
                            ml_ok = true;
                        } else {
                            // capture lexicographically smallest link string
                            auto cand = LT_strings_by_id[ltid];
                            if (cand < ml_link_sv) ml_link_sv = cand;
                        }
                    }
                }
                if (!ml_ok) continue;

                // ----- Probe movie_keyword: any row with keyword_id == k_target -----
                int32_t mk_lo = mk_off_p[mid];
                int32_t mk_hi = mk_off_p[mid + 1];
                bool mk_ok = false;
                for (int32_t r = mk_lo; r < mk_hi; r++) {
                    if (mk_kid[r] == k_target) { mk_ok = true; break; }
                }
                if (!mk_ok) continue;

                // ----- Probe movie_companies -----
                int32_t mc_lo = mc_off_p[mid];
                int32_t mc_hi = mc_off_p[mid + 1];

                std::string_view cand_cn;
                bool cand_cn_ok = false;
                for (int32_t r = mc_lo; r < mc_hi; r++) {
                    if (mc_ctid[r] != ct_target) continue;
                    // note IS NULL → offset[r] == offset[r+1] (empty varlen)
                    if (mc_noff[r] != mc_noff[r + 1]) continue;

                    int32_t cid = mc_cid[r];
                    // company_name row = cid - 1
                    size_t crow = (size_t)(cid - 1);
                    int16_t cc = cn_cc_p[crow];
                    if (cc == pl_code || cc == 0) continue;

                    uint64_t na = cn_noff_p[crow], nb = cn_noff_p[crow + 1];
                    size_t nlen = (size_t)(nb - na);
                    const char* ndata = cn_ndat_p + na;
                    bool name_ok =
                        (nlen >= nsub1_len && memmem(ndata, nlen, nsub1, nsub1_len)) ||
                        (nlen >= nsub2_len && memmem(ndata, nlen, nsub2, nsub2_len));
                    if (!name_ok) continue;

                    std::string_view sv(ndata, nlen);
                    if (!cand_cn_ok || sv < cand_cn) {
                        cand_cn = sv;
                        cand_cn_ok = true;
                    }
                }
                if (!cand_cn_ok) continue;

                // qualified tuple → update MIN aggregates locally
                int tid = omp_get_thread_num();
                Local& L = locals[(size_t)tid];

                std::string_view t_sv(tdat + a, tlen);
                if (!L.has_cn || std::string_view(cand_cn) < std::string_view(L.best_cn)) {
                    L.best_cn.assign(cand_cn.data(), cand_cn.size());
                    L.has_cn = true;
                }
                if (!L.has_lt || ml_link_sv < std::string_view(L.best_lt)) {
                    L.best_lt.assign(ml_link_sv.data(), ml_link_sv.size());
                    L.has_lt = true;
                }
                if (!L.has_t || t_sv < std::string_view(L.best_t)) {
                    L.best_t.assign(t_sv.data(), t_sv.size());
                    L.has_t = true;
                }
            }
        }

        // Reduce
        std::string best_cn, best_lt, best_t;
        bool has_cn = false, has_lt = false, has_t = false;
        for (auto& L : locals) {
            if (L.has_cn && (!has_cn || L.best_cn < best_cn)) { best_cn = L.best_cn; has_cn = true; }
            if (L.has_lt && (!has_lt || L.best_lt < best_lt)) { best_lt = L.best_lt; has_lt = true; }
            if (L.has_t  && (!has_t  || L.best_t  < best_t )) { best_t  = L.best_t;  has_t  = true; }
        }

        // -------------------------------------------------------------------
        // Phase: output
        // -------------------------------------------------------------------
        {
            GENDB_PHASE("output");
            std::string out = rdir + "/Q11b.csv";
            FILE* f = std::fopen(out.c_str(), "w");
            std::fprintf(f, "from_company,movie_link_type,sequel_movie\n");
            if (has_cn || has_lt || has_t) {
                std::fprintf(f, "%s,%s,%s\n",
                    has_cn ? best_cn.c_str() : "",
                    has_lt ? best_lt.c_str() : "",
                    has_t  ? best_t.c_str()  : "");
            }
            std::fclose(f);
        }
    }

    return 0;
}
