// Q9d - MIN(an.name), MIN(chn.name), MIN(n.name), MIN(t.title)
// Plan: role-driven scan via cast_info__role_id CSR for 'actress',
// filter note IN voice-set, female gender, aka_name exists, US movie company.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <climits>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_set>

#include "timing_utils.h"
#include "mmap_utils.h"

#ifdef _OPENMP
#include <omp.h>
#endif

using gendb::MmapColumn;

static std::string read_file_str(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    s.resize(sz);
    if (sz > 0) std::fread(&s[0], 1, sz, f);
    std::fclose(f);
    return s;
}

static void write_csv_field(std::string& out, std::string_view v) {
    out.push_back('"');
    for (char c : v) {
        if (c == '"') { out.push_back('"'); out.push_back('"'); }
        else out.push_back(c);
    }
    out.push_back('"');
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    mkdir(results_dir.c_str(), 0755);

    GENDB_PHASE("total");

    // ---- Resolve dictionary codes & load columns ----
    int32_t rt_id_actress = 0;
    int16_t us_code = 0;
    int8_t  f_code = 0;

    // Keep static so .dat strings live throughout the program
    static std::string rt_dat, cc_dat, g_dat;

    // Resolve actress
    {
        auto rt_off_vec = read_file_str(store + "/role_type/role.off");
        const int64_t* rt_off = reinterpret_cast<const int64_t*>(rt_off_vec.data());
        size_t rt_off_count = rt_off_vec.size() / sizeof(int64_t);
        rt_dat = read_file_str(store + "/role_type/role.dat");
        for (size_t i = 0; i + 1 < rt_off_count; ++i) {
            std::string_view sv(rt_dat.data() + rt_off[i], rt_off[i+1] - rt_off[i]);
            if (sv == "actress") { rt_id_actress = (int32_t)(i + 1); break; }
        }
    }

    // Resolve us_code
    {
        auto cc_off_vec = read_file_str(store + "/company_name/country_code.dict.off");
        const int64_t* cc_off = reinterpret_cast<const int64_t*>(cc_off_vec.data());
        size_t cc_off_count = cc_off_vec.size() / sizeof(int64_t);
        cc_dat = read_file_str(store + "/company_name/country_code.dict.dat");
        for (size_t i = 0; i + 1 < cc_off_count; ++i) {
            std::string_view sv(cc_dat.data() + cc_off[i], cc_off[i+1] - cc_off[i]);
            if (sv == "[us]") { us_code = (int16_t)(i + 1); break; }
        }
    }

    // Resolve f_code
    {
        auto g_off_vec = read_file_str(store + "/name/gender.dict.off");
        const int64_t* g_off = reinterpret_cast<const int64_t*>(g_off_vec.data());
        size_t g_off_count = g_off_vec.size() / sizeof(int64_t);
        g_dat = read_file_str(store + "/name/gender.dict.dat");
        for (size_t i = 0; i + 1 < g_off_count; ++i) {
            std::string_view sv(g_dat.data() + g_off[i], g_off[i+1] - g_off[i]);
            if (sv == "f") { f_code = (int8_t)(i + 1); break; }
        }
    }

    if (!rt_id_actress || !us_code || !f_code) {
        std::fprintf(stderr, "code resolution failed: rt=%d us=%d f=%d\n",
                     rt_id_actress, (int)us_code, (int)f_code);
        return 1;
    }

    // mmap data
    MmapColumn<int32_t> ci_role_id;
    MmapColumn<int32_t> ci_movie_id;
    MmapColumn<int32_t> ci_person_id;
    MmapColumn<int32_t> ci_person_role_id;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;

    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int8_t>  n_gender;

    MmapColumn<int32_t> mc_company_id;

    MmapColumn<int32_t> crid_off;
    MmapColumn<int32_t> crid_row;
    MmapColumn<int32_t> mc_movie_off;
    MmapColumn<int32_t> an_pid_off;

    // For MIN strings
    MmapColumn<int64_t> an_name_off;
    MmapColumn<char>    an_name_dat;
    MmapColumn<int64_t> chn_name_off;
    MmapColumn<char>    chn_name_dat;
    MmapColumn<int64_t> n_name_off;
    MmapColumn<char>    n_name_dat;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    {
        GENDB_PHASE("data_loading");
        ci_role_id.open(store + "/cast_info/role_id.bin");
        ci_movie_id.open(store + "/cast_info/movie_id.bin");
        ci_person_id.open(store + "/cast_info/person_id.bin");
        ci_person_role_id.open(store + "/cast_info/person_role_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        ci_note_dat.open(store + "/cast_info/note.dat");

        cn_country_code.open(store + "/company_name/country_code.bin");
        n_gender.open(store + "/name/gender.bin");

        mc_company_id.open(store + "/movie_companies/company_id.bin");

        crid_off.open(store + "/_idx/cast_info__role_id__offsets.bin");
        crid_row.open(store + "/_idx/cast_info__role_id__rowids.bin");
        mc_movie_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        an_pid_off.open(store + "/_idx/aka_name__person_id__offsets.bin");

        an_name_off.open(store + "/aka_name/name.off");
        an_name_dat.open(store + "/aka_name/name.dat");
        chn_name_off.open(store + "/char_name/name.off");
        chn_name_dat.open(store + "/char_name/name.dat");
        n_name_off.open(store + "/name/name.off");
        n_name_dat.open(store + "/name/name.dat");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        // Random-access columns
        ci_person_id.advise_random();
        ci_movie_id.advise_random();
        ci_person_role_id.advise_random();
        ci_note_off.advise_random();
        ci_note_dat.advise_random();
        n_gender.advise_random();
        cn_country_code.advise_random();
        mc_company_id.advise_random();
        mc_movie_off.advise_random();
        an_pid_off.advise_random();

        // Pre-touch CSR slice areas
        crid_off.prefetch();
    }

    // Voice-note IN-set
    static const std::string_view voice_notes[4] = {
        std::string_view("(voice)"),
        std::string_view("(voice: Japanese version)"),
        std::string_view("(voice) (uncredited)"),
        std::string_view("(voice: English version)")
    };

    // Determine slice
    int32_t lo = crid_off[rt_id_actress];
    int32_t hi = crid_off[rt_id_actress + 1];
    int32_t n_rows = hi - lo;

    // ---- Parallel main scan ----
    // Thread-local MINs (as string copies to detach lifetime from temporary computations,
    // though mmap data persists; we use string for safety in reduction).
    struct LocalMin {
        bool has_an = false, has_chn = false, has_n = false, has_t = false;
        std::string_view min_an, min_chn, min_n, min_t;
    };

    // Determine number of threads
    int nthreads = 1;

    {
        GENDB_PHASE("main_scan");

        std::vector<LocalMin> per_thread;
        #pragma omp parallel
        {
            #pragma omp single
            {
                #ifdef _OPENMP
                nthreads = omp_get_num_threads();
                #endif
                per_thread.resize(nthreads);
            }
        }

        #pragma omp parallel
        {
            int tid = 0;
            #ifdef _OPENMP
            tid = omp_get_thread_num();
            #endif
            LocalMin lm;

            const int32_t* __restrict__ crid_row_p = crid_row.data;
            const int32_t* __restrict__ ci_role_p = ci_role_id.data; (void)ci_role_p;
            const int32_t* __restrict__ ci_movie_p = ci_movie_id.data;
            const int32_t* __restrict__ ci_person_p = ci_person_id.data;
            const int32_t* __restrict__ ci_prole_p = ci_person_role_id.data;
            const int64_t* __restrict__ note_off_p = ci_note_off.data;
            const char*    __restrict__ note_dat_p = ci_note_dat.data;
            const int8_t*  __restrict__ ngen_p = n_gender.data;
            const int16_t* __restrict__ ccc_p = cn_country_code.data;
            const int32_t* __restrict__ mc_off_p = mc_movie_off.data;
            const int32_t* __restrict__ mc_comp_p = mc_company_id.data;
            const int32_t* __restrict__ an_off_p = an_pid_off.data;

            const int64_t* __restrict__ an_name_off_p = an_name_off.data;
            const char*    __restrict__ an_name_dat_p = an_name_dat.data;
            const int64_t* __restrict__ chn_name_off_p = chn_name_off.data;
            const char*    __restrict__ chn_name_dat_p = chn_name_dat.data;
            const int64_t* __restrict__ n_name_off_p = n_name_off.data;
            const char*    __restrict__ n_name_dat_p = n_name_dat.data;
            const int64_t* __restrict__ t_title_off_p = t_title_off.data;
            const char*    __restrict__ t_title_dat_p = t_title_dat.data;

            #pragma omp for schedule(dynamic, 4096)
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = crid_row_p[k];

                // Filter person_role_id != NULL
                int32_t chn_id = ci_prole_p[r];
                if (chn_id == INT32_MIN || chn_id <= 0) continue;

                // Filter note IN voice set
                int64_t no0 = note_off_p[r];
                int64_t no1 = note_off_p[r + 1];
                size_t nlen = (size_t)(no1 - no0);
                if (nlen == 0) continue;
                std::string_view note_sv(note_dat_p + no0, nlen);
                bool note_match = false;
                for (const auto& vn : voice_notes) {
                    if (note_sv == vn) { note_match = true; break; }
                }
                if (!note_match) continue;

                // Filter gender = 'f'
                int32_t pid = ci_person_p[r];
                if (pid <= 0) continue;
                if (ngen_p[pid - 1] != f_code) continue;

                // Filter aka_name exists for pid
                int32_t an_lo = an_off_p[pid];
                int32_t an_hi = an_off_p[pid + 1];
                if (an_lo >= an_hi) continue;

                // Filter US company via mc lookup
                int32_t mv = ci_movie_p[r];
                if (mv <= 0) continue;
                int32_t mc_lo = mc_off_p[mv];
                int32_t mc_hi = mc_off_p[mv + 1];
                bool us_match = false;
                for (int32_t mi = mc_lo; mi < mc_hi; ++mi) {
                    int32_t comp_id = mc_comp_p[mi];
                    if (comp_id > 0 && ccc_p[comp_id - 1] == us_code) {
                        us_match = true;
                        break;
                    }
                }
                if (!us_match) continue;

                // Row qualifies: update MINs
                // MIN(an.name): over all aka_name rows in [an_lo, an_hi)
                for (int32_t ai = an_lo; ai < an_hi; ++ai) {
                    int64_t o0 = an_name_off_p[ai];
                    int64_t o1 = an_name_off_p[ai + 1];
                    std::string_view s(an_name_dat_p + o0, (size_t)(o1 - o0));
                    if (!lm.has_an || s < lm.min_an) {
                        lm.min_an = s;
                        lm.has_an = true;
                    }
                }

                // MIN(chn.name) at row chn_id-1
                {
                    int64_t o0 = chn_name_off_p[chn_id - 1];
                    int64_t o1 = chn_name_off_p[chn_id];
                    std::string_view s(chn_name_dat_p + o0, (size_t)(o1 - o0));
                    if (!lm.has_chn || s < lm.min_chn) {
                        lm.min_chn = s;
                        lm.has_chn = true;
                    }
                }

                // MIN(n.name) at row pid-1
                {
                    int64_t o0 = n_name_off_p[pid - 1];
                    int64_t o1 = n_name_off_p[pid];
                    std::string_view s(n_name_dat_p + o0, (size_t)(o1 - o0));
                    if (!lm.has_n || s < lm.min_n) {
                        lm.min_n = s;
                        lm.has_n = true;
                    }
                }

                // MIN(t.title) at row mv-1
                {
                    int64_t o0 = t_title_off_p[mv - 1];
                    int64_t o1 = t_title_off_p[mv];
                    std::string_view s(t_title_dat_p + o0, (size_t)(o1 - o0));
                    if (!lm.has_t || s < lm.min_t) {
                        lm.min_t = s;
                        lm.has_t = true;
                    }
                }
            }

            per_thread[tid] = lm;
        }

        // Reduce
        LocalMin g;
        for (auto& lm : per_thread) {
            if (lm.has_an && (!g.has_an || lm.min_an < g.min_an)) { g.min_an = lm.min_an; g.has_an = true; }
            if (lm.has_chn && (!g.has_chn || lm.min_chn < g.min_chn)) { g.min_chn = lm.min_chn; g.has_chn = true; }
            if (lm.has_n && (!g.has_n || lm.min_n < g.min_n)) { g.min_n = lm.min_n; g.has_n = true; }
            if (lm.has_t && (!g.has_t || lm.min_t < g.min_t)) { g.min_t = lm.min_t; g.has_t = true; }
        }

        // ---- Output ----
        {
            GENDB_PHASE("output");
            std::string out_path = results_dir + "/Q9d.csv";
            FILE* f = std::fopen(out_path.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
            std::string row;
            row.reserve(1024);
            row.append("alternative_name,voiced_char_name,voicing_actress,american_movie\n");
            if (g.has_an) write_csv_field(row, g.min_an); else row.append("\"\"");
            row.push_back(',');
            if (g.has_chn) write_csv_field(row, g.min_chn); else row.append("\"\"");
            row.push_back(',');
            if (g.has_n) write_csv_field(row, g.min_n); else row.append("\"\"");
            row.push_back(',');
            if (g.has_t) write_csv_field(row, g.min_t); else row.append("\"\"");
            row.push_back('\n');
            std::fwrite(row.data(), 1, row.size(), f);
            std::fclose(f);
        }
    }

    (void)n_rows;
    return 0;
}
