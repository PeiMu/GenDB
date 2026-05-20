// Q21c — JOB benchmark
// Generated implementation following plan: drive movie_link from filtered link_type IDs.
#define _GNU_SOURCE
#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>

using namespace gendb;

static inline bool has_substr(const char* h, size_t hn, const char* n, size_t nn) {
    return ::memmem(h, hn, n, nn) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) { std::fprintf(stderr, "usage: q21c <storage> <results>\n"); return 1; }
    std::string store   = argv[1];
    std::string results = argv[2];
    mkdir(results.c_str(), 0755);

    // ---- Column mappings ----
    MmapColumn<int64_t> ct_kind_off;   MmapColumn<char> ct_kind_dat;
    MmapColumn<int32_t> ct_id;
    MmapColumn<int64_t> kw_off;        MmapColumn<char> kw_dat;
    MmapColumn<int32_t> kw_id;
    MmapColumn<int64_t> lt_off;        MmapColumn<char> lt_dat;
    MmapColumn<int32_t> lt_id;
    MmapColumn<int64_t> cn_name_off;   MmapColumn<char> cn_name_dat;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_cc_dict_off;MmapColumn<char> cn_cc_dict_dat;
    MmapColumn<int32_t> cn_id;
    MmapColumn<int32_t> ml_movie_id, ml_lt_id;
    MmapColumn<int32_t> mlt_off, mlt_row;
    MmapColumn<int32_t> mk_off_kw, mk_row_kw;
    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mi_off_mid;
    MmapColumn<int64_t> mi_info_off;   MmapColumn<char> mi_info_dat;
    MmapColumn<int32_t> mc_off_mid;
    MmapColumn<int32_t> mc_company_id, mc_ct_id;
    MmapColumn<int64_t> mc_note_off;   MmapColumn<char> mc_note_dat;
    MmapColumn<int32_t> t_production_year;
    MmapColumn<int64_t> t_title_off;   MmapColumn<char> t_title_dat;

    {
        GENDB_PHASE("data_loading");
        ct_kind_off.open(store + "/company_type/kind.off");
        ct_kind_dat.open(store + "/company_type/kind.dat");
        ct_id.open(store + "/company_type/id.bin");

        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");
        kw_id.open(store + "/keyword/id.bin");

        lt_off.open(store + "/link_type/link.off");
        lt_dat.open(store + "/link_type/link.dat");
        lt_id.open(store + "/link_type/id.bin");

        cn_name_off.open(store + "/company_name/name.off");
        cn_name_dat.open(store + "/company_name/name.dat");
        cn_cc.open(store + "/company_name/country_code.bin");
        cn_cc_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(store + "/company_name/country_code.dict.dat");
        cn_id.open(store + "/company_name/id.bin");

        ml_movie_id.open(store + "/movie_link/movie_id.bin");
        ml_lt_id.open(store + "/movie_link/link_type_id.bin");
        mlt_off.open(store + "/_idx/movie_link__link_type_id__offsets.bin");
        mlt_row.open(store + "/_idx/movie_link__link_type_id__rowids.bin");

        mk_off_kw.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_row_kw.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");

        mi_off_mid.open(store + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off.open(store + "/movie_info/info.off");
        mi_info_dat.open(store + "/movie_info/info.dat");

        mc_off_mid.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");
        mc_ct_id.open(store + "/movie_companies/company_type_id.bin");
        mc_note_off.open(store + "/movie_companies/note.off");
        mc_note_dat.open(store + "/movie_companies/note.dat");

        t_production_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");
    }

    // ---- Resolve dimension IDs ----
    int32_t ct_pc = -1;
    int32_t k_sequel = -1;
    int16_t cc_pl = -1;
    std::vector<int32_t> LT_ids;
    {
        GENDB_PHASE("resolve_dims");
        // ct_pc
        for (size_t i = 0; i < ct_id.size(); i++) {
            auto a = ct_kind_off[i], b = ct_kind_off[i+1];
            std::string_view s(ct_kind_dat.data + a, b - a);
            if (s == "production companies") { ct_pc = ct_id[i]; break; }
        }
        // k_sequel
        for (size_t i = 0; i < kw_id.size(); i++) {
            auto a = kw_off[i], b = kw_off[i+1];
            std::string_view s(kw_dat.data + a, b - a);
            if (s == "sequel") { k_sequel = kw_id[i]; break; }
        }
        // LT_ids: link LIKE '%follow%'
        for (size_t i = 0; i < lt_id.size(); i++) {
            auto a = lt_off[i], b = lt_off[i+1];
            if (has_substr(lt_dat.data + a, b - a, "follow", 6)) {
                LT_ids.push_back(lt_id[i]);
            }
        }
        // cc_pl (dictionary code for "[pl]")
        size_t n_cc = cn_cc_dict_off.size() - 1;
        for (size_t i = 0; i < n_cc; i++) {
            auto a = cn_cc_dict_off[i], b = cn_cc_dict_off[i+1];
            std::string_view s(cn_cc_dict_dat.data + a, b - a);
            if (s == "[pl]") { cc_pl = (int16_t)i; break; }
        }
        if (ct_pc < 0 || k_sequel < 0 || LT_ids.empty()) {
            std::fprintf(stderr, "dim resolution failed: ct_pc=%d k_sequel=%d LT=%zu\n",
                         ct_pc, k_sequel, LT_ids.size());
        }
    }

    // ---- Build CN_ids bitset (company_name LIKE '%Film%'|'%Warner%' AND country_code != '[pl]') ----
    const size_t cn_n = cn_id.size();
    std::vector<uint64_t> CN_ids(cn_n / 64 + 1, 0);
    {
        GENDB_PHASE("build_cn_bitset");
        for (size_t i = 0; i < cn_n; i++) {
            if (cn_cc[i] == cc_pl) continue;
            auto a = cn_name_off[i], b = cn_name_off[i+1];
            size_t len = b - a;
            const char* p = cn_name_dat.data + a;
            if (has_substr(p, len, "Film", 4) || has_substr(p, len, "Warner", 6)) {
                // dense PK: company_id = i+1, but we'll index bitset by company_id
                // Use i as bit index (we'll convert from cid later: cid-1).
                CN_ids[i >> 6] |= (uint64_t)1 << (i & 63);
            }
        }
    }
    auto cn_test = [&](int32_t cid) -> bool {
        // dense pk: row = cid - 1
        size_t r = (size_t)(cid - 1);
        if (r >= cn_n) return false;
        return (CN_ids[r >> 6] >> (r & 63)) & 1ULL;
    };

    // ---- Build seq_movies bitset via movie_keyword__keyword_id CSR for k_sequel ----
    const size_t title_n = t_production_year.size();  // == 2528312
    std::vector<uint64_t> seq_movies(title_n / 64 + 2, 0);
    {
        GENDB_PHASE("build_seq_movies_bitset");
        int32_t lo = mk_off_kw[k_sequel];
        int32_t hi = mk_off_kw[k_sequel + 1];
        for (int32_t k = lo; k < hi; k++) {
            int32_t r = mk_row_kw[k];
            int32_t mv = mk_movie_id[r];
            size_t b = (size_t)mv;
            if (b < title_n + 1) seq_movies[b >> 6] |= (uint64_t)1 << (b & 63);
        }
    }
    auto seq_test = [&](int32_t mv) -> bool {
        size_t b = (size_t)mv;
        return (seq_movies[b >> 6] >> (b & 63)) & 1ULL;
    };

    // ---- MI_set: 9 strings ----
    std::unordered_set<std::string_view> MI_set;
    MI_set.reserve(16);
    MI_set.insert("Sweden");
    MI_set.insert("Norway");
    MI_set.insert("Germany");
    MI_set.insert("Denmark");
    MI_set.insert("Swedish");
    MI_set.insert("Denish");
    MI_set.insert("Norwegian");
    MI_set.insert("German");
    MI_set.insert("English");

    // ---- Driver: walk ml rows for each LT_id; per-mv probes ----
    int32_t best_cn = -1;   // company_id of MIN(cn.name)
    int32_t best_lt = -1;   // link_type id of MIN(lt.link)
    int32_t best_mv = -1;   // movie id of MIN(t.title)

    auto cn_name_sv = [&](int32_t cid) -> std::string_view {
        size_t r = (size_t)(cid - 1);
        auto a = cn_name_off[r], b = cn_name_off[r+1];
        return std::string_view(cn_name_dat.data + a, b - a);
    };
    auto lt_link_sv = [&](int32_t ltid) -> std::string_view {
        size_t r = (size_t)(ltid - 1);
        auto a = lt_off[r], b = lt_off[r+1];
        return std::string_view(lt_dat.data + a, b - a);
    };
    auto title_sv = [&](int32_t mv) -> std::string_view {
        size_t r = (size_t)(mv - 1);
        auto a = t_title_off[r], b = t_title_off[r+1];
        return std::string_view(t_title_dat.data + a, b - a);
    };

    {
        GENDB_PHASE("main_scan");
        for (int32_t ltid : LT_ids) {
            int32_t lo = mlt_off[ltid], hi = mlt_off[ltid + 1];
            for (int32_t k = lo; k < hi; k++) {
                int32_t r = mlt_row[k];
                int32_t mv = ml_movie_id[r];
                if (mv < 1 || (size_t)mv > title_n) continue;

                // Year filter
                int32_t py = t_production_year[mv - 1];
                if (py == INT32_MIN) continue;
                if (py < 1950 || py > 2010) continue;

                // sequel filter
                if (!seq_test(mv)) continue;

                // movie_info EXISTS scan
                int32_t mi_lo = mi_off_mid[mv - 1], mi_hi = mi_off_mid[mv];
                bool mi_ok = false;
                for (int32_t j = mi_lo; j < mi_hi; j++) {
                    auto a = mi_info_off[j], b = mi_info_off[j+1];
                    std::string_view info(mi_info_dat.data + a, b - a);
                    if (MI_set.find(info) != MI_set.end()) { mi_ok = true; break; }
                }
                if (!mi_ok) continue;

                // movie_companies scan
                int32_t mc_lo = mc_off_mid[mv - 1], mc_hi = mc_off_mid[mv];
                bool mc_ok = false;
                for (int32_t j = mc_lo; j < mc_hi; j++) {
                    // note IS NULL ⇔ empty varlen
                    auto na = mc_note_off[j], nb = mc_note_off[j+1];
                    if (nb != na) continue;
                    if (mc_ct_id[j] != ct_pc) continue;
                    int32_t cid = mc_company_id[j];
                    if (!cn_test(cid)) continue;
                    mc_ok = true;
                    // Update MIN(cn.name)
                    std::string_view name = cn_name_sv(cid);
                    if (best_cn < 0 || name < cn_name_sv(best_cn)) {
                        best_cn = cid;
                    }
                }
                if (!mc_ok) continue;

                // Update MIN(lt.link)
                std::string_view link = lt_link_sv(ltid);
                if (best_lt < 0 || link < lt_link_sv(best_lt)) best_lt = ltid;

                // Update MIN(t.title)
                std::string_view tt = title_sv(mv);
                if (best_mv < 0 || tt < title_sv(best_mv)) best_mv = mv;
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q21c.csv";
        FILE* f = fopen(outpath.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 1; }
        fprintf(f, "company_name,link_type,western_follow_up\n");
        if (best_cn > 0 && best_lt > 0 && best_mv > 0) {
            auto cn_s = cn_name_sv(best_cn);
            auto lt_s = lt_link_sv(best_lt);
            auto tt_s = title_sv(best_mv);
            fwrite(cn_s.data(), 1, cn_s.size(), f);
            fputc(',', f);
            fwrite(lt_s.data(), 1, lt_s.size(), f);
            fputc(',', f);
            fwrite(tt_s.data(), 1, tt_s.size(), f);
            fputc('\n', f);
        }
        fclose(f);
    }

    (void)argc; (void)argv;
    return 0;
}
