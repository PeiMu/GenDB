// Q10a: MIN(chn.name) AS uncredited_voiced_character, MIN(t.title) AS russian_movie
// Strategy: russian-driven path via cn -> mc CSR -> dedup movie_ids -> filter year -> ci offsets -> filters -> chn.name

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static std::string read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); std::exit(1); }
    struct stat st; fstat(fd, &st);
    std::string s; s.resize(st.st_size);
    ssize_t got = 0; size_t off = 0;
    while (off < (size_t)st.st_size) {
        got = ::read(fd, s.data()+off, st.st_size-off);
        if (got <= 0) break;
        off += got;
    }
    ::close(fd);
    return s;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) { fprintf(stderr, "usage: q10a <gendb_dir> <results_dir>\n"); return 1; }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    // Open all mmaps + dict reads
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mccid_off;
    MmapColumn<int32_t> mccid_row;
    MmapColumn<int32_t> ci_role_id;
    MmapColumn<int32_t> ci_person_role_id;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<int32_t> cimid_off;
    MmapColumn<int32_t> t_prod_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<int64_t> chn_name_off;

    std::string cc_dat;
    std::vector<int64_t> cc_off_vec;
    std::string rt_dat;
    std::vector<int64_t> rt_off_vec;

    // ci_note_dat, t_title_dat, chn_name_dat: keep mmapped raw bytes
    int fd_ci_note = -1, fd_t_title = -1, fd_chn_name = -1;
    const char* ci_note_dat = nullptr; size_t ci_note_dat_sz = 0;
    const char* t_title_dat = nullptr; size_t t_title_dat_sz = 0;
    const char* chn_name_dat = nullptr; size_t chn_name_dat_sz = 0;

    int16_t ru_code = 0;
    int32_t rt_id_actor = 0;

    {
        GENDB_PHASE("data_loading");
        // company_name dict + country_code column
        {
            int fd = ::open((store + "/company_name/country_code.dict.off").c_str(), O_RDONLY);
            struct stat st; fstat(fd, &st);
            cc_off_vec.resize(st.st_size / sizeof(int64_t));
            ::read(fd, cc_off_vec.data(), st.st_size);
            ::close(fd);
        }
        cc_dat = read_file(store + "/company_name/country_code.dict.dat");
        cn_cc.open(store + "/company_name/country_code.bin");

        // movie_companies CSR
        mccid_off.open(store + "/_idx/movie_companies__company_id__offsets.bin");
        mccid_row.open(store + "/_idx/movie_companies__company_id__rowids.bin");
        mc_movie_id.open(store + "/movie_companies/movie_id.bin");

        // role_type
        {
            int fd = ::open((store + "/role_type/role.off").c_str(), O_RDONLY);
            struct stat st; fstat(fd, &st);
            rt_off_vec.resize(st.st_size / sizeof(int64_t));
            ::read(fd, rt_off_vec.data(), st.st_size);
            ::close(fd);
        }
        rt_dat = read_file(store + "/role_type/role.dat");

        // cast_info columns
        ci_role_id.open(store + "/cast_info/role_id.bin");
        ci_person_role_id.open(store + "/cast_info/person_role_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        cimid_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");

        // ci note dat (mmap)
        {
            fd_ci_note = ::open((store + "/cast_info/note.dat").c_str(), O_RDONLY);
            struct stat st; fstat(fd_ci_note, &st);
            ci_note_dat_sz = st.st_size;
            if (ci_note_dat_sz > 0) {
                void* p = mmap(nullptr, ci_note_dat_sz, PROT_READ, MAP_PRIVATE, fd_ci_note, 0);
                ci_note_dat = (const char*)p;
                madvise((void*)p, ci_note_dat_sz, MADV_RANDOM);
            }
        }

        // title
        t_prod_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        {
            fd_t_title = ::open((store + "/title/title.dat").c_str(), O_RDONLY);
            struct stat st; fstat(fd_t_title, &st);
            t_title_dat_sz = st.st_size;
            if (t_title_dat_sz > 0) {
                void* p = mmap(nullptr, t_title_dat_sz, PROT_READ, MAP_PRIVATE, fd_t_title, 0);
                t_title_dat = (const char*)p;
                madvise((void*)p, t_title_dat_sz, MADV_RANDOM);
            }
        }

        // char_name
        chn_name_off.open(store + "/char_name/name.off");
        {
            fd_chn_name = ::open((store + "/char_name/name.dat").c_str(), O_RDONLY);
            struct stat st; fstat(fd_chn_name, &st);
            chn_name_dat_sz = st.st_size;
            if (chn_name_dat_sz > 0) {
                void* p = mmap(nullptr, chn_name_dat_sz, PROT_READ, MAP_PRIVATE, fd_chn_name, 0);
                chn_name_dat = (const char*)p;
                madvise((void*)p, chn_name_dat_sz, MADV_RANDOM);
            }
        }

        // resolve dict codes
        for (size_t i = 0; i+1 < cc_off_vec.size(); ++i) {
            std::string_view sv(cc_dat.data() + cc_off_vec[i], cc_off_vec[i+1] - cc_off_vec[i]);
            if (sv == "[ru]") { ru_code = (int16_t)(i+1); break; }
        }
        for (size_t i = 0; i+1 < rt_off_vec.size(); ++i) {
            std::string_view sv(rt_dat.data() + rt_off_vec[i], rt_off_vec[i+1] - rt_off_vec[i]);
            if (sv == "actor") { rt_id_actor = (int32_t)(i+1); break; }
        }
    }

    // Aggregation state
    std::string_view min_chn_name; bool have_chn = false;
    std::string_view min_t_title;  bool have_title = false;

    {
        GENDB_PHASE("main_scan");

        // Step 1: build russian movie_id set
        // company_id is 1-based; cn_cc indexed by (cid-1). company_name length = 234997, cid range 1..234997.
        // Russian companies: cn_cc[cid-1] == ru_code.
        // Use CSR mccid_off indexed by cid: lo=mccid_off[cid], hi=mccid_off[cid+1]
        const size_t num_companies = cn_cc.count;
        std::vector<int32_t> ru_movies;
        ru_movies.reserve(8192);

        for (size_t cid = 1; cid <= num_companies; ++cid) {
            if (cn_cc.data[cid-1] != ru_code) continue;
            int32_t lo = mccid_off.data[cid];
            int32_t hi = mccid_off.data[cid+1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = mccid_row.data[k];
                int32_t mv = mc_movie_id.data[r];
                ru_movies.push_back(mv);
            }
        }
        // dedup
        std::sort(ru_movies.begin(), ru_movies.end());
        ru_movies.erase(std::unique(ru_movies.begin(), ru_movies.end()), ru_movies.end());

        const char* voice_needle = "(voice)";
        const size_t voice_len = 7;
        const char* uncr_needle = "(uncredited)";
        const size_t uncr_len = 12;

        // Step 2: for each russian movie_id, check year > 2005, iterate ci offsets
        const int32_t* py = t_prod_year.data;
        const int64_t* tt_off = t_title_off.data;
        const int32_t* ci_role = ci_role_id.data;
        const int32_t* ci_prid = ci_person_role_id.data;
        const int64_t* cn_off = ci_note_off.data;
        const int32_t* ci_off_arr = cimid_off.data;
        const int64_t* chn_off = chn_name_off.data;

        for (int32_t mv : ru_movies) {
            // year check
            int32_t yr = py[mv-1];
            if (yr <= 2005) continue; // INT32_MIN also excluded

            int32_t ci_lo = ci_off_arr[mv];
            int32_t ci_hi = ci_off_arr[mv+1];

            std::string_view t_title_view;
            bool t_view_built = false;

            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                if (ci_role[r] != rt_id_actor) continue;
                int32_t prid = ci_prid[r];
                if (prid == INT32_MIN) continue;

                // note LIKE both
                int64_t a = cn_off[r], b = cn_off[r+1];
                if (b - a < (int64_t)voice_len) continue;
                if (!memmem(ci_note_dat + a, b - a, voice_needle, voice_len)) continue;
                if (!memmem(ci_note_dat + a, b - a, uncr_needle, uncr_len)) continue;

                // Passed all filters. Update aggregates.
                // chn.name at chn_id-1 = prid - 1
                int64_t ca = chn_off[prid-1], cb = chn_off[prid];
                std::string_view chn_sv(chn_name_dat + ca, cb - ca);
                if (!have_chn || chn_sv < min_chn_name) {
                    min_chn_name = chn_sv;
                    have_chn = true;
                }

                if (!t_view_built) {
                    int64_t ta = tt_off[mv-1], tb = tt_off[mv];
                    t_title_view = std::string_view(t_title_dat + ta, tb - ta);
                    t_view_built = true;
                }
                if (!have_title || t_title_view < min_t_title) {
                    min_t_title = t_title_view;
                    have_title = true;
                }
            }
        }
    }

    // Output
    {
        GENDB_PHASE("output");
        // ensure results_dir exists
        mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q10a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) { perror(out_path.c_str()); return 1; }
        std::fprintf(fp, "uncredited_voiced_character,russian_movie\n");
        if (have_chn && have_title) {
            std::fwrite(min_chn_name.data(), 1, min_chn_name.size(), fp);
            std::fputc(',', fp);
            std::fwrite(min_t_title.data(), 1, min_t_title.size(), fp);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
