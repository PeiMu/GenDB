// Q10b - JOB
// SELECT MIN(chn.name), MIN(t.title)
// FROM char_name, cast_info, company_name, company_type, movie_companies,
//      role_type, title
// WHERE ci.note LIKE '%(producer)%'
//   AND cn.country_code='[ru]' AND rt.role='actor'
//   AND t.production_year > 2010
//   AND t.id=mc.movie_id AND t.id=ci.movie_id AND ci.movie_id=mc.movie_id
//   AND chn.id=ci.person_role_id AND rt.id=ci.role_id
//   AND cn.id=mc.company_id AND ct.id=mc.company_type_id;
//
// Strategy: drive from RU companies -> mc CSR -> candidate movie bitset
//  -> filter year>2010 -> walk cast_info offsets -> filter role=actor +
//  note LIKE '%(producer)%' -> deref char_name for MIN.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static std::string slurp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("open " + path);
    struct stat st;
    if (fstat(fd, &st) < 0) { ::close(fd); throw std::runtime_error("stat " + path); }
    std::string s;
    s.resize(st.st_size);
    ssize_t got = ::read(fd, s.data(), st.st_size);
    (void)got;
    ::close(fd);
    return s;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    int32_t rt_id_actor = 0;
    int16_t ru_code = 0;

    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mccid_off;
    MmapColumn<int32_t> mccid_row;
    MmapColumn<int32_t> title_py;
    MmapColumn<int64_t> title_off;
    MmapColumn<char>    title_dat;
    MmapColumn<int32_t> cimid_off;
    MmapColumn<int32_t> ci_role_id;
    MmapColumn<int32_t> ci_prid;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;
    MmapColumn<int64_t> chn_name_off;
    MmapColumn<char>    chn_name_dat;

    {
        GENDB_PHASE("data_loading");

        // Resolve role_type.role = 'actor'
        {
            MmapColumn<int64_t> rt_off(store + "/role_type/role.off");
            std::string rt_dat = slurp(store + "/role_type/role.dat");
            for (size_t i = 0; i + 1 < rt_off.count; ++i) {
                size_t lo = rt_off[i], hi = rt_off[i+1];
                if (hi - lo == 5 && std::memcmp(rt_dat.data() + lo, "actor", 5) == 0) {
                    rt_id_actor = (int32_t)(i + 1);
                    break;
                }
            }
        }

        // Resolve company_name.country_code dict code for '[ru]'
        {
            MmapColumn<int64_t> cc_off(store + "/company_name/country_code.dict.off");
            std::string cc_dat = slurp(store + "/company_name/country_code.dict.dat");
            for (size_t i = 0; i + 1 < cc_off.count; ++i) {
                size_t lo = cc_off[i], hi = cc_off[i+1];
                if (hi - lo == 4 && std::memcmp(cc_dat.data() + lo, "[ru]", 4) == 0) {
                    ru_code = (int16_t)(i + 1);
                    break;
                }
            }
        }

        cn_cc.open(store + "/company_name/country_code.bin");
        mc_movie_id.open(store + "/movie_companies/movie_id.bin");
        mccid_off.open(store + "/_idx/movie_companies__company_id__offsets.bin");
        mccid_row.open(store + "/_idx/movie_companies__company_id__rowids.bin");
        title_py.open(store + "/title/production_year.bin");
        title_off.open(store + "/title/title.off");
        title_dat.open(store + "/title/title.dat");
        cimid_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");
        ci_role_id.open(store + "/cast_info/role_id.bin");
        ci_prid.open(store + "/cast_info/person_role_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        ci_note_dat.open(store + "/cast_info/note.dat");
        chn_name_off.open(store + "/char_name/name.off");
        chn_name_dat.open(store + "/char_name/name.dat");

        // Random-access hints for tables that we'll probe at random rows.
        ci_role_id.advise_random();
        ci_prid.advise_random();
        ci_note_off.advise_random();
        ci_note_dat.advise_random();
        chn_name_off.advise_random();
        chn_name_dat.advise_random();
        title_py.advise_random();
        title_off.advise_random();
        title_dat.advise_random();
        mc_movie_id.advise_random();

        // Sequential streamed-once items:
        cimid_off.advise_sequential();
    }

    // If predicate values don't exist, the result is two NULLs.
    auto write_empty = [&]() {
        std::ofstream out(results_dir + "/Q10b.csv");
        out << "uncredited_voiced_character,russian_mov_with_actor_producer\n,\n";
    };

    if (rt_id_actor == 0 || ru_code == 0) {
        write_empty();
        return 0;
    }

    // Build ru_company_ids
    std::vector<int32_t> ru_company_ids;
    ru_company_ids.reserve(512);
    for (size_t i = 0; i < cn_cc.count; ++i) {
        if (cn_cc[i] == ru_code) ru_company_ids.push_back((int32_t)(i + 1));
    }

    const size_t n_title = title_py.count;
    std::vector<uint8_t> cand(n_title, 0);

    {
        GENDB_PHASE("dim_filter");
        for (int32_t cid : ru_company_ids) {
            int32_t lo = mccid_off[cid];
            int32_t hi = mccid_off[cid + 1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = mccid_row[k];
                int32_t mv = mc_movie_id[r];
                if (mv >= 1 && (size_t)mv <= n_title) cand[mv - 1] = 1;
            }
        }
    }

    // Per-thread MIN holders.
    int max_th = omp_get_max_threads();
    std::vector<std::string> tl_min_chn(max_th);
    std::vector<std::string> tl_min_title(max_th);
    std::vector<uint8_t>     tl_has_chn(max_th, 0);
    std::vector<uint8_t>     tl_has_title(max_th, 0);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string& min_chn   = tl_min_chn[tid];
            std::string& min_title = tl_min_title[tid];
            uint8_t&     has_chn   = tl_has_chn[tid];
            uint8_t&     has_title = tl_has_title[tid];

            #pragma omp for schedule(dynamic, 4096)
            for (size_t mv0 = 0; mv0 < n_title; ++mv0) {
                if (!cand[mv0]) continue;
                int32_t py = title_py[mv0];
                if (py <= 2010) continue;          // also rejects INT32_MIN (null)

                int32_t mv = (int32_t)(mv0 + 1);
                int32_t lo = cimid_off[mv];
                int32_t hi = cimid_off[mv + 1];
                if (lo >= hi) continue;

                bool any_match = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (ci_role_id[r] != rt_id_actor) continue;
                    int32_t prid = ci_prid[r];
                    if (prid <= 0 || prid == INT32_MIN) continue;

                    int64_t no_lo = ci_note_off[r];
                    int64_t no_hi = ci_note_off[r + 1];
                    size_t  nlen  = (size_t)(no_hi - no_lo);
                    if (nlen < 10) continue;       // "(producer)" is 10 chars
                    if (!memmem(ci_note_dat.data + no_lo, nlen, "(producer)", 10)) continue;

                    // chn deref
                    int64_t clo = chn_name_off[prid - 1];
                    int64_t chi = chn_name_off[prid];
                    std::string_view cn(chn_name_dat.data + clo, (size_t)(chi - clo));
                    if (!has_chn || cn < std::string_view(min_chn)) {
                        min_chn.assign(cn);
                        has_chn = 1;
                    }
                    any_match = true;
                }

                if (any_match) {
                    int64_t tlo = title_off[mv0];
                    int64_t thi = title_off[mv0 + 1];
                    std::string_view tt(title_dat.data + tlo, (size_t)(thi - tlo));
                    if (!has_title || tt < std::string_view(min_title)) {
                        min_title.assign(tt);
                        has_title = 1;
                    }
                }
            }
        }
    }

    // Reduce per-thread MINs.
    std::string final_chn, final_title;
    bool got_chn = false, got_title = false;
    for (int t = 0; t < max_th; ++t) {
        if (tl_has_chn[t]) {
            if (!got_chn || tl_min_chn[t] < final_chn) {
                final_chn = tl_min_chn[t];
                got_chn = true;
            }
        }
        if (tl_has_title[t]) {
            if (!got_title || tl_min_title[t] < final_title) {
                final_title = tl_min_title[t];
                got_title = true;
            }
        }
    }

    {
        GENDB_PHASE("output");
        std::ofstream out(results_dir + "/Q10b.csv");
        out << "uncredited_voiced_character,russian_mov_with_actor_producer\n";
        // Note: values are strings; if they contained commas/quotes we'd need
        // CSV escaping, but JOB string columns don't.
        if (got_chn)   out << final_chn;
        out << ",";
        if (got_title) out << final_title;
        out << "\n";
    }

    return 0;
}
