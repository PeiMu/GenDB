// Q10c — MIN(chn.name), MIN(t.title)
// Driver: cast_info linear scan (sorted by movie_id), filter by note LIKE '%(producer)%'
// Per qualifying row: title.production_year[mv-1] > 1990 AND mc has US producer.
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static std::string read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    struct stat st; ::stat(path.c_str(), &st);
    std::string buf;
    buf.resize(st.st_size);
    if (st.st_size > 0) std::fread(buf.data(), 1, st.st_size, f);
    std::fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---- resolve us_code ----
    int16_t us_code = 0;
    {
        GENDB_PHASE("resolve_us_code");
        MmapColumn<int64_t> cc_off(store + "/company_name/country_code.dict.off");
        std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
        for (size_t i = 0; i + 1 < cc_off.count; ++i) {
            size_t lo = cc_off.data[i], hi = cc_off.data[i + 1];
            if (hi - lo == 4 && std::memcmp(cc_dat.data() + lo, "[us]", 4) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }
        if (us_code == 0) { std::fprintf(stderr, "[us] not found\n"); return 2; }
    }

    // ---- mmap columns ----
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> t_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;
    MmapColumn<int64_t> chn_name_off;
    MmapColumn<char>    chn_name_dat;
    MmapColumn<int32_t> ci_movie_id;
    MmapColumn<int32_t> ci_prole;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;
    MmapColumn<int32_t> mc_movie_off;
    MmapColumn<int32_t> mc_company_id;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(store + "/company_name/country_code.bin");
        t_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");
        chn_name_off.open(store + "/char_name/name.off");
        chn_name_dat.open(store + "/char_name/name.dat");
        ci_movie_id.open(store + "/cast_info/movie_id.bin");
        ci_prole.open(store + "/cast_info/person_role_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        ci_note_dat.open(store + "/cast_info/note.dat");
        mc_movie_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");

        cn_cc.advise_random();
        t_year.advise_random();
        chn_name_off.advise_random();
        chn_name_dat.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
        mc_movie_off.advise_random();
        mc_company_id.advise_random();

        ci_movie_id.advise_sequential();
        ci_prole.advise_sequential();
        ci_note_off.advise_sequential();
        ci_note_dat.advise_sequential();
    }

    const size_t N = ci_movie_id.count;
    const int32_t MAX_TITLE_ID = (int32_t)t_year.count;
    const int32_t MAX_COMPANY_ID = (int32_t)cn_cc.count;
    const int32_t MAX_CHN_ID = (int32_t)(chn_name_off.count > 0 ? chn_name_off.count - 1 : 0);

    // Best per-thread.
    int nthreads = omp_get_max_threads();
    std::vector<std::string> best_chn(nthreads), best_title(nthreads);

    {
        GENDB_PHASE("main_scan");
        const char* needle = "(producer)";
        const size_t needle_len = 10;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string local_best_chn;
            std::string local_best_title;
            bool has_chn = false, has_title = false;

            int32_t last_mv = -1;
            bool last_mv_ok = false;

            #pragma omp for schedule(static, 65536) nowait
            for (size_t r = 0; r < N; ++r) {
                int32_t prole = ci_prole.data[r];
                if (prole == INT32_MIN) continue;

                int64_t no = ci_note_off.data[r];
                int64_t no_next = ci_note_off.data[r + 1];
                size_t nlen = (size_t)(no_next - no);
                if (nlen < needle_len) continue;

                const void* hit = ::memmem(ci_note_dat.data + no, nlen, needle, needle_len);
                if (!hit) continue;

                int32_t mv = ci_movie_id.data[r];
                if (mv <= 0 || mv > MAX_TITLE_ID) continue;
                int32_t py = t_year.data[mv - 1];
                if (py == INT32_MIN || py <= 1990) continue;

                bool mv_ok;
                if (mv == last_mv) {
                    mv_ok = last_mv_ok;
                } else {
                    mv_ok = false;
                    if (mv >= 0 && mv + 1 < (int32_t)mc_movie_off.count) {
                        int32_t lo = mc_movie_off.data[mv];
                        int32_t hi = mc_movie_off.data[mv + 1];
                        for (int32_t k = lo; k < hi; ++k) {
                            int32_t cid = mc_company_id.data[k];
                            if (cid > 0 && cid <= MAX_COMPANY_ID) {
                                if (cn_cc.data[cid - 1] == us_code) { mv_ok = true; break; }
                            }
                        }
                    }
                    last_mv = mv;
                    last_mv_ok = mv_ok;
                }
                if (!mv_ok) continue;

                // chn.name[prole-1]
                if (prole > 0 && prole <= MAX_CHN_ID) {
                    int64_t co = chn_name_off.data[prole - 1];
                    int64_t co_next = chn_name_off.data[prole];
                    size_t clen = (size_t)(co_next - co);
                    std::string_view sv(chn_name_dat.data + co, clen);
                    if (!has_chn || sv < std::string_view(local_best_chn)) {
                        local_best_chn.assign(sv);
                        has_chn = true;
                    }
                }
                // title[mv-1]
                {
                    int64_t to = t_title_off.data[mv - 1];
                    int64_t to_next = t_title_off.data[mv];
                    size_t tlen = (size_t)(to_next - to);
                    std::string_view sv(t_title_dat.data + to, tlen);
                    if (!has_title || sv < std::string_view(local_best_title)) {
                        local_best_title.assign(sv);
                        has_title = true;
                    }
                }
            }

            best_chn[tid] = has_chn ? std::move(local_best_chn) : std::string();
            best_title[tid] = has_title ? std::move(local_best_title) : std::string();
        }
    }

    // Reduce.
    std::string final_chn, final_title;
    bool has_chn = false, has_title = false;
    for (int i = 0; i < nthreads; ++i) {
        if (!best_chn[i].empty()) {
            if (!has_chn || best_chn[i] < final_chn) { final_chn = best_chn[i]; has_chn = true; }
        }
        if (!best_title[i].empty()) {
            if (!has_title || best_title[i] < final_title) { final_title = best_title[i]; has_title = true; }
        }
    }

    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q10c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 3; }
        std::fprintf(f, "uncredited_voiced_character,movie_with_american_producer\n");
        std::fprintf(f, "%s,%s\n",
                     has_chn   ? final_chn.c_str()   : "",
                     has_title ? final_title.c_str() : "");
        std::fclose(f);
    }
    return 0;
}
