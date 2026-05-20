// Q15d: SELECT MIN(at1.title), MIN(t.title)
//   FROM aka_title at1, company_name cn, company_type ct, info_type it1,
//        keyword k, movie_companies mc, movie_info mi, movie_keyword mk, title t
//   WHERE cn.country_code='[us]' AND it1.info='release dates'
//     AND mi.note LIKE '%internet%' AND t.production_year > 1990
//     AND mi/mk/mc/at1 joined on movie_id == title.id
//
// Driver: mi via CSR(info_type_id) — iterate only mi rows with info_type_id == it1_id.
// Per surviving mi row: existence checks on mc(US co.), mk, then per-at1 MIN aggregation.
// Title is dense-PK: production_year[mv-1] direct probe.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"
#include "date_utils.h"

using namespace gendb;

static std::string path_join(const std::string& a, const std::string& b) {
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

// Read entire file into a std::string (used for dict .dat files).
static std::string read_whole_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st; fstat(fd, &st);
    std::string s;
    s.resize(st.st_size);
    if (st.st_size > 0) {
        ssize_t got = ::read(fd, s.data(), st.st_size);
        if (got != st.st_size) { ::close(fd); throw std::runtime_error("short read " + path); }
    }
    ::close(fd);
    return s;
}

template<typename T>
static std::vector<T> read_vec(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st; fstat(fd, &st);
    std::vector<T> v(st.st_size / sizeof(T));
    if (st.st_size > 0) {
        ssize_t got = ::read(fd, v.data(), st.st_size);
        if (got != st.st_size) { ::close(fd); throw std::runtime_error("short read " + path); }
    }
    ::close(fd);
    return v;
}

// Per-thread aggregation state: two MIN(string) accumulators.
struct ThreadAgg {
    bool has_at1 = false;
    std::string_view min_at1_title;
    bool has_t = false;
    std::string_view min_t_title;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--key value ...]\n", argv[0]);
        return 1;
    }
    init_date_tables();
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];

    // Parameters (defaults match Q15d).
    std::string p_cn_cc = parse_string_arg(argc, argv, "--cn_country_code", std::string("[us]"));
    std::string p_it_info = parse_string_arg(argc, argv, "--it_info", std::string("release dates"));
    std::string p_note_like = parse_string_arg(argc, argv, "--mi_note_like", std::string("internet"));
    int64_t p_year_gt = parse_int_arg(argc, argv, "--prod_year_gt", 1990);

    {
        std::string mkcmd = "mkdir -p '" + results_dir + "'";
        (void)system(mkcmd.c_str());
    }

    GENDB_PHASE("total");

    // -------- Load all required columns/indexes --------
    MmapColumn<int16_t> cn_cc_col;
    MmapColumn<int64_t> it_info_off, mi_note_off, at1_title_off, title_title_off;
    MmapColumn<char>    it_info_dat, mi_note_dat, at1_title_dat, title_title_dat;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id, mc_company_id;
    MmapColumn<int32_t> title_prod_year;
    MmapColumn<int32_t> mi_it_off, mi_it_rowids;
    MmapColumn<int32_t> mc_off_idx, mk_off_idx, at1_off_idx;

    // Dict .off for country_code (small, read into vector).
    std::vector<int64_t> cc_dict_off_vec;
    std::string cc_dict_dat_str;

    {
        GENDB_PHASE("data_loading");
        // company_name: dict for country_code + column
        cc_dict_off_vec = read_vec<int64_t>(path_join(gendb_dir, "company_name/country_code.dict.off"));
        cc_dict_dat_str = read_whole_file(path_join(gendb_dir, "company_name/country_code.dict.dat"));
        cn_cc_col.open(path_join(gendb_dir, "company_name/country_code.bin"));

        // info_type
        it_info_off.open(path_join(gendb_dir, "info_type/info.off"));
        it_info_dat.open(path_join(gendb_dir, "info_type/info.dat"));

        // movie_info
        mi_movie_id.open(path_join(gendb_dir, "movie_info/movie_id.bin"));
        mi_info_type_id.open(path_join(gendb_dir, "movie_info/info_type_id.bin"));
        mi_note_off.open(path_join(gendb_dir, "movie_info/note.off"));
        mi_note_dat.open(path_join(gendb_dir, "movie_info/note.dat"));

        // title
        title_prod_year.open(path_join(gendb_dir, "title/production_year.bin"));
        title_title_off.open(path_join(gendb_dir, "title/title.off"));
        title_title_dat.open(path_join(gendb_dir, "title/title.dat"));

        // movie_companies
        mc_company_id.open(path_join(gendb_dir, "movie_companies/company_id.bin"));

        // aka_title
        at1_title_off.open(path_join(gendb_dir, "aka_title/title.off"));
        at1_title_dat.open(path_join(gendb_dir, "aka_title/title.dat"));

        // Indexes
        mi_it_off.open(path_join(gendb_dir, "_idx/movie_info__info_type_id__offsets.bin"));
        mi_it_rowids.open(path_join(gendb_dir, "_idx/movie_info__info_type_id__rowids.bin"));
        mc_off_idx.open(path_join(gendb_dir, "_idx/movie_companies__movie_id__offsets.bin"));
        mk_off_idx.open(path_join(gendb_dir, "_idx/movie_keyword__movie_id__offsets.bin"));
        at1_off_idx.open(path_join(gendb_dir, "_idx/aka_title__movie_id__offsets.bin"));

        // Random-access hints for varlen .dat probed by row index.
        mi_note_dat.advise_random();
        at1_title_dat.advise_random();
        title_title_dat.advise_random();

        // Prefetch hot fact columns / indexes.
        mi_movie_id.prefetch();
        mi_info_type_id.prefetch();
        mc_company_id.prefetch();
        mc_off_idx.prefetch();
        mk_off_idx.prefetch();
        at1_off_idx.prefetch();
        title_prod_year.prefetch();
        cn_cc_col.prefetch();
        mi_note_off.prefetch();
    }

    // -------- Resolve scalars --------
    int16_t us_code = 0;  // 0 means not found
    {
        for (size_t i = 0; i + 1 < cc_dict_off_vec.size(); ++i) {
            int64_t lo = cc_dict_off_vec[i];
            int64_t hi = cc_dict_off_vec[i + 1];
            std::string_view sv(cc_dict_dat_str.data() + lo, (size_t)(hi - lo));
            if (sv == p_cn_cc) { us_code = (int16_t)(i + 1); break; }
        }
    }

    int32_t it1_id = -1;
    {
        const int64_t* off = it_info_off.data;
        const char*    dat = it_info_dat.data;
        size_t nrows = it_info_off.count - 1;
        const char* needle = p_it_info.data();
        size_t nlen = p_it_info.size();
        for (size_t i = 0; i < nrows; ++i) {
            int64_t lo = off[i];
            int64_t hi = off[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len == nlen && std::memcmp(dat + lo, needle, nlen) == 0) {
                it1_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    ThreadAgg global;

    if (us_code != 0 && it1_id >= 0) {
        // Range of mi rows with info_type_id == it1_id.
        // CSR offsets are 0-based at slot 0..N inclusive; entries indexed by id.
        // From guide: lo = mi_it_off[it1_id], hi = mi_it_off[it1_id+1] (id is 1-based,
        // offsets file has N+1 = 114 entries; file has 115 int32 (padding) — safe to index it1_id+1).
        const int32_t lo = mi_it_off.data[it1_id];
        const int32_t hi = mi_it_off.data[it1_id + 1];
        const int32_t total = (hi > lo) ? (hi - lo) : 0;

        const int32_t* mi_rowids = mi_it_rowids.data;
        const int32_t* mi_mv     = mi_movie_id.data;
        const int64_t* mi_no_off = mi_note_off.data;
        const char*    mi_no_dat = mi_note_dat.data;
        const int32_t* t_year    = title_prod_year.data;
        const int64_t* t_to      = title_title_off.data;
        const char*    t_td      = title_title_dat.data;
        const int32_t* mc_off    = mc_off_idx.data;
        const int32_t* mc_cid    = mc_company_id.data;
        const int16_t* cn_cc     = cn_cc_col.data;
        const int32_t* mk_off    = mk_off_idx.data;
        const int32_t* at1_off   = at1_off_idx.data;
        const int64_t* at1_to    = at1_title_off.data;
        const char*    at1_td    = at1_title_dat.data;

        const char* needle = p_note_like.data();
        const size_t nlen = p_note_like.size();
        const int32_t year_thresh = (int32_t)p_year_gt;
        const int16_t target_cc = us_code;

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 12;
        // Cap threads at total work.
        if (total == 0) hw = 1;
        else if ((int32_t)hw > total) hw = (unsigned)total;

        std::vector<ThreadAgg> per_thread(hw);
        std::vector<std::thread> threads;
        threads.reserve(hw);

        GENDB_PHASE_MS("main_scan", main_ms);
        (void)main_ms;

        for (unsigned t = 0; t < hw; ++t) {
            threads.emplace_back([&, t]() {
                // Even partition of [lo, hi)
                int32_t per = total / (int32_t)hw;
                int32_t rem = total % (int32_t)hw;
                int32_t t_lo = lo + (int32_t)t * per + std::min((int32_t)t, rem);
                int32_t t_hi = t_lo + per + ((int32_t)t < rem ? 1 : 0);

                ThreadAgg local;

                for (int32_t k = t_lo; k < t_hi; ++k) {
                    int32_t r = mi_rowids[k];

                    // mi.note LIKE '%internet%'
                    int64_t n_lo = mi_no_off[r];
                    int64_t n_hi = mi_no_off[r + 1];
                    size_t  n_len = (size_t)(n_hi - n_lo);
                    if (n_len < nlen) continue;
                    if (memmem(mi_no_dat + n_lo, n_len, needle, nlen) == nullptr) continue;

                    int32_t mv = mi_mv[r];
                    if (mv <= 0) continue;

                    // title.production_year > 1990 && NOT NULL
                    int32_t yr = t_year[mv - 1];
                    if (yr == INT32_MIN || yr <= year_thresh) continue;

                    // mc semi-join: any mc row for mv with cn.country_code == us_code
                    int32_t mc_lo = mc_off[mv];
                    int32_t mc_hi = mc_off[mv + 1];
                    bool mc_ok = false;
                    for (int32_t j = mc_lo; j < mc_hi; ++j) {
                        int32_t cid = mc_cid[j];
                        if (cid >= 1 && cn_cc[cid - 1] == target_cc) { mc_ok = true; break; }
                    }
                    if (!mc_ok) continue;

                    // mk existence semi-join
                    int32_t mk_lo = mk_off[mv];
                    int32_t mk_hi = mk_off[mv + 1];
                    if (mk_lo >= mk_hi) continue;

                    // aka_title INNER: skip if no at1 rows for mv
                    int32_t a_lo = at1_off[mv];
                    int32_t a_hi = at1_off[mv + 1];
                    if (a_lo >= a_hi) continue;

                    // For each at1 row, update MIN(at1.title)
                    for (int32_t j = a_lo; j < a_hi; ++j) {
                        int64_t alo = at1_to[j];
                        int64_t ahi = at1_to[j + 1];
                        std::string_view av(at1_td + alo, (size_t)(ahi - alo));
                        if (!local.has_at1 || av < local.min_at1_title) {
                            local.has_at1 = true;
                            local.min_at1_title = av;
                        }
                    }

                    // Update MIN(t.title) once per surviving mv (each at1 row joins this title,
                    // but t.title is identical for all those copies, so a single update suffices).
                    int64_t tlo = t_to[mv - 1];
                    int64_t thi = t_to[mv];
                    std::string_view tv(t_td + tlo, (size_t)(thi - tlo));
                    if (!local.has_t || tv < local.min_t_title) {
                        local.has_t = true;
                        local.min_t_title = tv;
                    }
                }

                per_thread[t] = local;
            });
        }
        for (auto& th : threads) th.join();

        for (auto& l : per_thread) {
            if (l.has_at1 && (!global.has_at1 || l.min_at1_title < global.min_at1_title)) {
                global.has_at1 = true; global.min_at1_title = l.min_at1_title;
            }
            if (l.has_t && (!global.has_t || l.min_t_title < global.min_t_title)) {
                global.has_t = true; global.min_t_title = l.min_t_title;
            }
        }
    }

    // -------- Output --------
    {
        GENDB_PHASE("output");
        std::string out_path = path_join(results_dir, "Q15d.csv");
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 2; }
        std::fputs("aka_title,internet_movie_title\n", f);

        auto write_csv_field = [&](std::string_view s) {
            bool need_quote = false;
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (!need_quote) {
                std::fwrite(s.data(), 1, s.size(), f);
            } else {
                std::fputc('"', f);
                for (char c : s) {
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            }
        };

        if (global.has_at1) write_csv_field(global.min_at1_title);
        std::fputc(',', f);
        if (global.has_t) write_csv_field(global.min_t_title);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
