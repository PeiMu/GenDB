// Q8d: MIN(an1.name) AS costume_designer_pseudo, MIN(t.title) AS movie_with_costumes
// FROM aka_name, cast_info, company_name, movie_companies, name, role_type, title
// WHERE cn.country_code='[us]' AND rt.role='costume designer'
//   AND join chain (aka_name.person_id = name.id = cast_info.person_id,
//                   cast_info.movie_id = title.id = mc.movie_id, mc.company_id = cn.id,
//                   cast_info.role_id = rt.id)

#include "timing_utils.h"
#include "mmap_utils.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <limits>
#include <omp.h>

namespace fs = std::filesystem;
using gendb::MmapColumn;

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    fs::create_directories(results);

    // ---------------- data loading ----------------
    MmapColumn<int64_t> rt_off, ti_off, an_off;
    MmapColumn<char>    rt_dat, ti_dat, an_dat;
    MmapColumn<int64_t> cc_dict_off;
    MmapColumn<char>    cc_dict_dat;
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> ci_role, ci_movie, ci_person;
    MmapColumn<int32_t> mc_company;
    MmapColumn<int32_t> crid_off, crid_row;
    MmapColumn<int32_t> mcmid_off;
    MmapColumn<int32_t> akpid_off;

    int32_t rt_id_cd = 0;
    int16_t us_code = 0;

    {
        GENDB_PHASE("data_loading");

        rt_off.open(store + "/role_type/role.off");
        rt_dat.open(store + "/role_type/role.dat");

        cc_dict_off.open(store + "/company_name/country_code.dict.off");
        cc_dict_dat.open(store + "/company_name/country_code.dict.dat");
        cn_cc.open(store + "/company_name/country_code.bin");

        ci_role.open(store + "/cast_info/role_id.bin");
        ci_movie.open(store + "/cast_info/movie_id.bin");
        ci_person.open(store + "/cast_info/person_id.bin");

        mc_company.open(store + "/movie_companies/company_id.bin");

        crid_off.open(store + "/_idx/cast_info__role_id__offsets.bin");
        crid_row.open(store + "/_idx/cast_info__role_id__rowids.bin");

        mcmid_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        akpid_off.open(store + "/_idx/aka_name__person_id__offsets.bin");

        ti_off.open(store + "/title/title.off");
        ti_dat.open(store + "/title/title.dat");

        an_off.open(store + "/aka_name/name.off");
        an_dat.open(store + "/aka_name/name.dat");

        // Resolve rt_id for "costume designer"
        {
            std::string_view target("costume designer");
            for (size_t i = 0; i + 1 < rt_off.size(); ++i) {
                int64_t a = rt_off[i], b = rt_off[i + 1];
                std::string_view sv(rt_dat.data + a, (size_t)(b - a));
                if (sv == target) { rt_id_cd = (int32_t)(i + 1); break; }
            }
        }
        // Resolve us_code for "[us]"
        {
            std::string_view target("[us]");
            for (size_t i = 0; i + 1 < cc_dict_off.size(); ++i) {
                int64_t a = cc_dict_off[i], b = cc_dict_off[i + 1];
                std::string_view sv(cc_dict_dat.data + a, (size_t)(b - a));
                if (sv == target) { us_code = (int16_t)(i + 1); break; }
            }
        }
    }

    if (rt_id_cd == 0 || us_code == 0) {
        std::fprintf(stderr, "Failed to resolve rt_id_cd or us_code\n");
        return 1;
    }

    // ---------------- main scan ----------------
    int32_t lo = crid_off[rt_id_cd];
    int32_t hi = crid_off[rt_id_cd + 1];

    const int32_t* __restrict crid_row_p = crid_row.data;
    const int32_t* __restrict ci_movie_p = ci_movie.data;
    const int32_t* __restrict ci_person_p = ci_person.data;
    const int32_t* __restrict mcmid_off_p = mcmid_off.data;
    const int32_t* __restrict mc_company_p = mc_company.data;
    const int32_t* __restrict akpid_off_p = akpid_off.data;
    const int16_t* __restrict cn_cc_p = cn_cc.data;
    const int64_t* __restrict ti_off_p = ti_off.data;
    const char*    __restrict ti_dat_p = ti_dat.data;
    const int64_t* __restrict an_off_p = an_off.data;
    const char*    __restrict an_dat_p = an_dat.data;

    auto title_sv = [&](int32_t row_idx) -> std::string_view {
        int64_t a = ti_off_p[row_idx], b = ti_off_p[row_idx + 1];
        return std::string_view(ti_dat_p + a, (size_t)(b - a));
    };
    auto aname_sv = [&](int32_t row_idx) -> std::string_view {
        int64_t a = an_off_p[row_idx], b = an_off_p[row_idx + 1];
        return std::string_view(an_dat_p + a, (size_t)(b - a));
    };

    int32_t global_best_title_row = -1;
    int32_t global_best_aname_row = -1;

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel
        {
            int32_t local_title = -1;
            int32_t local_aname = -1;
            // Cache string_views to avoid repeated off lookups for current best
            std::string_view local_title_sv;
            std::string_view local_aname_sv;

            #pragma omp for schedule(static) nowait
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = crid_row_p[k];
                int32_t mv = ci_movie_p[r];
                int32_t pid = ci_person_p[r];

                // Aka existence range
                int32_t a_lo = akpid_off_p[pid];
                int32_t a_hi = akpid_off_p[pid + 1];
                if (a_lo == a_hi) continue;

                // MC range for movie_id
                int32_t m_lo = mcmid_off_p[mv];
                int32_t m_hi = mcmid_off_p[mv + 1];
                if (m_lo == m_hi) continue;

                // Semi-join: any mc row has cn.country_code[company_id-1] == us_code
                bool us_match = false;
                for (int32_t j = m_lo; j < m_hi; ++j) {
                    int32_t cid = mc_company_p[j];
                    if (cn_cc_p[cid - 1] == us_code) { us_match = true; break; }
                }
                if (!us_match) continue;

                // Update MIN(t.title): title row = mv - 1
                int32_t title_row = mv - 1;
                if (local_title < 0) {
                    local_title = title_row;
                    local_title_sv = title_sv(title_row);
                } else if (title_row != local_title) {
                    std::string_view cand = title_sv(title_row);
                    if (cand < local_title_sv) {
                        local_title = title_row;
                        local_title_sv = cand;
                    }
                }

                // Update MIN(an1.name) over the aka_name range
                for (int32_t j = a_lo; j < a_hi; ++j) {
                    if (local_aname < 0) {
                        local_aname = j;
                        local_aname_sv = aname_sv(j);
                    } else if (j != local_aname) {
                        std::string_view cand = aname_sv(j);
                        if (cand < local_aname_sv) {
                            local_aname = j;
                            local_aname_sv = cand;
                        }
                    }
                }
            }

            #pragma omp critical
            {
                if (local_title >= 0) {
                    if (global_best_title_row < 0 ||
                        title_sv(local_title) < title_sv(global_best_title_row)) {
                        global_best_title_row = local_title;
                    }
                }
                if (local_aname >= 0) {
                    if (global_best_aname_row < 0 ||
                        aname_sv(local_aname) < aname_sv(global_best_aname_row)) {
                        global_best_aname_row = local_aname;
                    }
                }
            }
        }
    }

    // ---------------- output ----------------
    {
        GENDB_PHASE("output");

        std::string out_path = results + "/Q8d.csv";
        std::ofstream out(out_path);
        out << "costume_designer_pseudo,movie_with_costumes\n";

        auto write_csv_field = [&](std::ostream& os, std::string_view s) {
            bool need_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (!need_quote) {
                os.write(s.data(), s.size());
            } else {
                os.put('"');
                for (char c : s) {
                    if (c == '"') os.put('"');
                    os.put(c);
                }
                os.put('"');
            }
        };

        if (global_best_aname_row >= 0) write_csv_field(out, aname_sv(global_best_aname_row));
        out << ",";
        if (global_best_title_row >= 0) write_csv_field(out, title_sv(global_best_title_row));
        out << "\n";
    }

    return 0;
}
