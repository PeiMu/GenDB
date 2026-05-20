// Q27a — JOB
// SELECT MIN(cn.name), MIN(lt.link), MIN(t.title)
// FROM 12 tables ... with semi-join structure.
//
// Driver: movie_keyword CSR for keyword='sequel' (very small)
// Probes: title.year, movie_link, movie_companies, complete_cast, movie_info

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <omp.h>

using gendb::MmapColumn;
namespace fs = std::filesystem;

static inline std::string_view varlen_at(const uint64_t* off, const char* dat, int64_t idx) {
    uint64_t s = off[idx];
    uint64_t e = off[idx + 1];
    return std::string_view(dat + s, e - s);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---------------- Data loading ----------------
    // Dim columns
    MmapColumn<char>     cct_off_raw, cct_dat_raw;
    MmapColumn<uint64_t> cct_off;
    MmapColumn<char>     cct_dat;
    MmapColumn<int32_t>  cct_id;

    MmapColumn<uint64_t> ct_off;
    MmapColumn<char>     ct_dat;
    MmapColumn<int32_t>  ct_id;

    MmapColumn<uint64_t> kw_off;
    MmapColumn<char>     kw_dat;
    MmapColumn<int32_t>  kw_id;

    MmapColumn<uint64_t> lt_off;
    MmapColumn<char>     lt_dat;
    MmapColumn<int32_t>  lt_id_col;

    MmapColumn<int16_t>  cn_cc;
    MmapColumn<uint64_t> cn_cc_dict_off;
    MmapColumn<char>     cn_cc_dict_dat;
    MmapColumn<uint64_t> cn_name_off;
    MmapColumn<char>     cn_name_dat;

    // Title
    MmapColumn<int32_t>  t_id_col;
    MmapColumn<int32_t>  t_year;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    // Facts (we use offsets indexes for range probes)
    MmapColumn<int32_t>  mk_movie_id;
    MmapColumn<int32_t>  mk_kid_off;        // /_idx/movie_keyword__keyword_id__offsets.bin
    MmapColumn<int32_t>  mk_kid_rowids;     // /_idx/movie_keyword__keyword_id__rowids.bin

    MmapColumn<int32_t>  ml_mid_off;
    MmapColumn<int32_t>  ml_lt;

    MmapColumn<int32_t>  mc_mid_off;
    MmapColumn<int32_t>  mc_ctid;
    MmapColumn<int32_t>  mc_cid;
    MmapColumn<uint64_t> mc_note_off;       // for note IS NULL test

    MmapColumn<int32_t>  cc_mid_off;
    MmapColumn<int32_t>  cc_sub;
    MmapColumn<int32_t>  cc_stat;

    MmapColumn<int32_t>  mi_mid_off;
    MmapColumn<uint64_t> mi_info_off;
    MmapColumn<char>     mi_info_dat;

    {
        GENDB_PHASE("data_loading");

        cct_off.open(gendb_dir + "/comp_cast_type/kind.off");
        cct_dat.open(gendb_dir + "/comp_cast_type/kind.dat");
        cct_id.open(gendb_dir + "/comp_cast_type/id.bin");

        ct_off.open(gendb_dir + "/company_type/kind.off");
        ct_dat.open(gendb_dir + "/company_type/kind.dat");
        ct_id.open(gendb_dir + "/company_type/id.bin");

        kw_off.open(gendb_dir + "/keyword/keyword.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");
        kw_id.open(gendb_dir + "/keyword/id.bin");

        lt_off.open(gendb_dir + "/link_type/link.off");
        lt_dat.open(gendb_dir + "/link_type/link.dat");
        lt_id_col.open(gendb_dir + "/link_type/id.bin");

        cn_cc.open(gendb_dir + "/company_name/country_code.bin");
        cn_cc_dict_off.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(gendb_dir + "/company_name/country_code.dict.dat");
        cn_name_off.open(gendb_dir + "/company_name/name.off");
        cn_name_dat.open(gendb_dir + "/company_name/name.dat");

        t_id_col.open(gendb_dir + "/title/id.bin");
        t_year.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        mk_movie_id.open(gendb_dir + "/movie_keyword/movie_id.bin");
        mk_kid_off.open(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_kid_rowids.open(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");

        ml_mid_off.open(gendb_dir + "/_idx/movie_link__movie_id__offsets.bin");
        ml_lt.open(gendb_dir + "/movie_link/link_type_id.bin");

        mc_mid_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_ctid.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_cid.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_note_off.open(gendb_dir + "/movie_companies/note.off");

        cc_mid_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        cc_sub.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_stat.open(gendb_dir + "/complete_cast/status_id.bin");

        mi_mid_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");

        // Hint the large random-access columns
        mc_note_off.advise_random();
        mi_info_off.advise_random();
        mc_ctid.advise_random();
        mc_cid.advise_random();
        cc_sub.advise_random();
        cc_stat.advise_random();
        ml_lt.advise_random();
        mi_info_dat.advise_random();
        t_year.advise_random();
    }

    // ---------------- Resolve dims ----------------
    int32_t cast_id = -1, crew_id = -1, complete_id = -1;
    {
        GENDB_PHASE("dim_resolve_cct");
        for (size_t i = 0; i < cct_id.size(); ++i) {
            auto sv = varlen_at(cct_off.data, cct_dat.data, (int64_t)i);
            int32_t id = cct_id.data[i];
            if (sv == "cast") cast_id = id;
            else if (sv == "crew") crew_id = id;
            else if (sv == "complete") complete_id = id;
        }
    }

    int32_t ct_prod_id = -1;
    {
        GENDB_PHASE("dim_resolve_ct");
        for (size_t i = 0; i < ct_id.size(); ++i) {
            auto sv = varlen_at(ct_off.data, ct_dat.data, (int64_t)i);
            if (sv == "production companies") { ct_prod_id = ct_id.data[i]; break; }
        }
    }

    int32_t sequel_kw_id = -1;
    {
        GENDB_PHASE("dim_resolve_kw");
        // Linear scan for keyword='sequel'
        size_t N = kw_id.size();
        const uint64_t* koff = kw_off.data;
        const char* kdat = kw_dat.data;
        for (size_t i = 0; i < N; ++i) {
            uint64_t s = koff[i], e = koff[i+1];
            if (e - s == 6 && memcmp(kdat + s, "sequel", 6) == 0) {
                sequel_kw_id = kw_id.data[i];
                break;
            }
        }
    }

    // lt_set: ids where link contains 'follow' (memmem)
    int32_t lt_set[16]; int lt_n = 0;
    {
        GENDB_PHASE("dim_resolve_lt");
        size_t N = lt_id_col.size();
        for (size_t i = 0; i < N; ++i) {
            auto sv = varlen_at(lt_off.data, lt_dat.data, (int64_t)i);
            if (memmem(sv.data(), sv.size(), "follow", 6) != nullptr) {
                lt_set[lt_n++] = lt_id_col.data[i];
            }
        }
    }

    // Resolve [pl] code
    int16_t pl_code = -1;
    {
        GENDB_PHASE("dim_resolve_pl_code");
        size_t N = cn_cc_dict_off.size() - 1; // number of dict entries
        for (size_t i = 0; i < N; ++i) {
            auto sv = varlen_at(cn_cc_dict_off.data, cn_cc_dict_dat.data, (int64_t)i);
            if (sv == "[pl]") { pl_code = (int16_t)i; break; }
        }
    }

    // Build cn_set bitset: country_code != pl_code AND (name contains 'Film' OR 'Warner')
    // company_name has dense PK 1..234997
    const size_t CN_N = 234997;
    std::vector<uint64_t> cn_bits((CN_N + 64) / 64 + 1, 0);
    {
        GENDB_PHASE("dim_resolve_cn");
        const int16_t* ccp = cn_cc.data;
        const uint64_t* noff = cn_name_off.data;
        const char* ndat = cn_name_dat.data;
        for (size_t i = 0; i < CN_N; ++i) {
            if (ccp[i] == pl_code) continue;
            uint64_t s = noff[i], e = noff[i+1];
            size_t L = e - s;
            const char* p = ndat + s;
            bool match = (memmem(p, L, "Film", 4) != nullptr) ||
                         (memmem(p, L, "Warner", 6) != nullptr);
            if (match) {
                // bitset indexed by cn_id (1-based). Row i has cn_id = i+1.
                size_t bit = i + 1;
                cn_bits[bit >> 6] |= (uint64_t(1) << (bit & 63));
            }
        }
    }

    auto cn_test = [&](int32_t cn_id) -> bool {
        if (cn_id <= 0 || (size_t)cn_id > CN_N) return false;
        size_t bit = (size_t)cn_id;
        return (cn_bits[bit >> 6] >> (bit & 63)) & 1;
    };

    if (sequel_kw_id < 0 || ct_prod_id < 0 || cast_id < 0 || crew_id < 0 ||
        complete_id < 0 || pl_code < 0 || lt_n == 0) {
        std::fprintf(stderr, "Dim resolution failed: sequel=%d ct_prod=%d cast=%d crew=%d complete=%d pl=%d lt_n=%d\n",
            sequel_kw_id, ct_prod_id, cast_id, crew_id, complete_id, (int)pl_code, lt_n);
    }

    // ---------------- Driver: CSR for keyword=sequel ----------------
    int32_t drv_lo = mk_kid_off.data[sequel_kw_id];
    int32_t drv_hi = mk_kid_off.data[sequel_kw_id + 1];
    int32_t drv_count = drv_hi - drv_lo;

    // Globals for MIN
    std::string g_min_cn_name;     bool g_has_cn = false;
    std::string g_min_lt_link;     bool g_has_lt = false;
    std::string g_min_t_title;     bool g_has_t  = false;

    {
        GENDB_PHASE("main_scan");

        // Per-thread mins (use a small number)
        int nthreads = omp_get_max_threads();
        if (nthreads > 12) nthreads = 12;

        struct Local {
            std::string cn, lt, tt;
            bool h_cn=false, h_lt=false, h_t=false;
            char pad[64];
        };
        std::vector<Local> locals(nthreads);

        // Capture lt_set into closure
        int32_t lt_set_local[16];
        for (int i = 0; i < lt_n; ++i) lt_set_local[i] = lt_set[i];
        int lt_n_local = lt_n;

        #pragma omp parallel for schedule(dynamic, 64) num_threads(nthreads)
        for (int32_t k = drv_lo; k < drv_hi; ++k) {
            int tid_omp = omp_get_thread_num();
            Local& L = locals[tid_omp];

            int32_t mk_row = mk_kid_rowids.data[k];
            int32_t t_id = mk_movie_id.data[mk_row];
            if (t_id <= 0) continue;

            // title year check (production_year row index = t_id - 1)
            int32_t yr = t_year.data[t_id - 1];
            if (yr < 1950 || yr > 2000) continue;

            // movie_link probe: find min link string among matching rows
            std::string_view best_lt_link;
            bool has_lt = false;
            {
                int32_t lo = ml_mid_off.data[t_id];
                int32_t hi = ml_mid_off.data[t_id + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    int32_t ltv = ml_lt.data[r];
                    bool in = false;
                    for (int i = 0; i < lt_n_local; ++i) {
                        if (lt_set_local[i] == ltv) { in = true; break; }
                    }
                    if (!in) continue;
                    auto sv = varlen_at(lt_off.data, lt_dat.data, (int64_t)(ltv - 1));
                    if (!has_lt || sv < best_lt_link) {
                        best_lt_link = sv;
                        has_lt = true;
                    }
                }
            }
            if (!has_lt) continue;

            // movie_companies probe
            std::string_view best_cn_name;
            bool has_cn = false;
            {
                int32_t lo = mc_mid_off.data[t_id];
                int32_t hi = mc_mid_off.data[t_id + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    if (mc_ctid.data[r] != ct_prod_id) continue;
                    // note IS NULL: off[r] == off[r+1]
                    if (mc_note_off.data[r] != mc_note_off.data[r + 1]) continue;
                    int32_t cid = mc_cid.data[r];
                    if (!cn_test(cid)) continue;
                    auto sv = varlen_at(cn_name_off.data, cn_name_dat.data, (int64_t)(cid - 1));
                    if (!has_cn || sv < best_cn_name) {
                        best_cn_name = sv;
                        has_cn = true;
                    }
                }
            }
            if (!has_cn) continue;

            // complete_cast probe
            bool cc_match = false;
            {
                int32_t lo = cc_mid_off.data[t_id];
                int32_t hi = cc_mid_off.data[t_id + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    int32_t sid = cc_sub.data[r];
                    if (sid != cast_id && sid != crew_id) continue;
                    if (cc_stat.data[r] != complete_id) continue;
                    cc_match = true;
                    break;
                }
            }
            if (!cc_match) continue;

            // movie_info probe — info IN ('Sweden','Germany','Swedish','German')
            bool mi_match = false;
            {
                int32_t lo = mi_mid_off.data[t_id];
                int32_t hi = mi_mid_off.data[t_id + 1];
                const uint64_t* moff = mi_info_off.data;
                const char* mdat = mi_info_dat.data;
                for (int32_t r = lo; r < hi; ++r) {
                    uint64_t s = moff[r], e = moff[r + 1];
                    uint64_t L = e - s;
                    if (L < 6 || L > 7) continue;
                    const char* p = mdat + s;
                    if (L == 6) {
                        if (memcmp(p, "Sweden", 6) == 0 || memcmp(p, "German", 6) == 0) {
                            mi_match = true; break;
                        }
                    } else {
                        if (memcmp(p, "Germany", 7) == 0 || memcmp(p, "Swedish", 7) == 0) {
                            mi_match = true; break;
                        }
                    }
                }
            }
            if (!mi_match) continue;

            // All semi-joins passed for this t_id. Update local mins.
            auto t_title_sv = varlen_at(t_title_off.data, t_title_dat.data, (int64_t)(t_id - 1));

            if (!L.h_cn || std::string_view(L.cn) > best_cn_name) {
                L.cn.assign(best_cn_name.data(), best_cn_name.size());
                L.h_cn = true;
            }
            if (!L.h_lt || std::string_view(L.lt) > best_lt_link) {
                L.lt.assign(best_lt_link.data(), best_lt_link.size());
                L.h_lt = true;
            }
            if (!L.h_t || std::string_view(L.tt) > t_title_sv) {
                L.tt.assign(t_title_sv.data(), t_title_sv.size());
                L.h_t = true;
            }
        }

        // Merge per-thread locals
        for (auto& L : locals) {
            if (L.h_cn && (!g_has_cn || L.cn < g_min_cn_name)) {
                g_min_cn_name = L.cn; g_has_cn = true;
            }
            if (L.h_lt && (!g_has_lt || L.lt < g_min_lt_link)) {
                g_min_lt_link = L.lt; g_has_lt = true;
            }
            if (L.h_t && (!g_has_t || L.tt < g_min_t_title)) {
                g_min_t_title = L.tt; g_has_t = true;
            }
        }

        (void)drv_count;
    }

    // ---------------- Output ----------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q27a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) { std::fprintf(stderr, "Cannot open output %s\n", out_path.c_str()); return 1; }
        std::fprintf(fp, "producing_company,link_type,complete_western_sequel\n");
        // CSV escape: wrap in quotes if needed
        auto write_field = [&](const std::string& s, bool last) {
            bool need_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (need_quote) {
                std::fputc('"', fp);
                for (char c : s) {
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
            } else {
                std::fwrite(s.data(), 1, s.size(), fp);
            }
            std::fputc(last ? '\n' : ',', fp);
        };
        write_field(g_has_cn ? g_min_cn_name : std::string(), false);
        write_field(g_has_lt ? g_min_lt_link : std::string(), false);
        write_field(g_has_t  ? g_min_t_title : std::string(), true);
        std::fclose(fp);
    }

    return 0;
}
