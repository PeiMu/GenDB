// Q33c — JOB benchmark
// Driver: movie_link. For each ml row in lt_allowed_ids, lookup t1/t2 attrs
// then probe mi_idx1, mi_idx2, mc1, mc2 via primary CSR on movie_id.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "cli_params.h"

// ------------------------------------------------------------- mmap helpers
struct Mmap {
    const void* ptr = nullptr;
    size_t size = 0;
    int fd = -1;
    void open_file(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "open failed: %s\n", path.c_str()); std::abort(); }
        struct stat st; fstat(fd, &st);
        size = st.st_size;
        if (size == 0) { ptr = nullptr; return; }
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::abort(); }
        ptr = p;
    }
    ~Mmap() {
        if (ptr && size) munmap(const_cast<void*>(ptr), size);
        if (fd >= 0) ::close(fd);
    }
    template<typename T> const T* as() const { return reinterpret_cast<const T*>(ptr); }
};

static inline std::string_view sv_from(const char* data, const int64_t* off, size_t i) {
    return std::string_view(data + off[i], off[i+1] - off[i]);
}

static inline int sv_cmp(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    int r = std::memcmp(a.data(), b.data(), n);
    if (r != 0) return r;
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) { fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [--params...]\n", argv[0]); return 1; }
    std::string gendb = argv[1];
    std::string results_dir = argv[2];

    // Parse parameters (defaults from params.json)
    int32_t prod_lo = (int32_t)gendb::parse_int_arg(argc, argv, "--production_year_lower", 2000);
    int32_t prod_hi = (int32_t)gendb::parse_int_arg(argc, argv, "--production_year_upper", 2010);
    std::string info_upper = gendb::parse_string_arg(argc, argv, "--info_upper", "3.5");
    std::string info_eq    = gendb::parse_string_arg(argc, argv, "--info_eq", "rating");
    std::string info_eq_2  = gendb::parse_string_arg(argc, argv, "--info_eq_2", "rating");
    std::string country_neq = gendb::parse_string_arg(argc, argv, "--country_code_neq", "[us]");

    // --------------------------------- mmap files ---------------------------------
    Mmap m_ml_movie_id, m_ml_linked, m_ml_lt;
    Mmap m_t_kind, m_t_prod_year, m_t_title_off, m_t_title_dat;
    Mmap m_mii_movie_off, m_mii_info_type, m_mii_info_off, m_mii_info_dat;
    Mmap m_mc_movie_off, m_mc_company_id;
    Mmap m_cn_country, m_cn_name_off, m_cn_name_dat;
    Mmap m_cn_dict_off, m_cn_dict_dat;
    Mmap m_kt_kind_off, m_kt_kind_dat, m_kt_id;
    Mmap m_lt_link_off, m_lt_link_dat, m_lt_id;
    Mmap m_it_info_off, m_it_info_dat, m_it_id;

    {
        GENDB_PHASE("data_loading");
        m_ml_movie_id.open_file(gendb + "/movie_link/movie_id.bin");
        m_ml_linked.open_file(gendb + "/movie_link/linked_movie_id.bin");
        m_ml_lt.open_file(gendb + "/movie_link/link_type_id.bin");

        m_t_kind.open_file(gendb + "/title/kind_id.bin");
        m_t_prod_year.open_file(gendb + "/title/production_year.bin");
        m_t_title_off.open_file(gendb + "/title/title.off");
        m_t_title_dat.open_file(gendb + "/title/title.dat");

        m_mii_movie_off.open_file(gendb + "/_idx/movie_info_idx__movie_id__offsets.bin");
        m_mii_info_type.open_file(gendb + "/movie_info_idx/info_type_id.bin");
        m_mii_info_off.open_file(gendb + "/movie_info_idx/info.off");
        m_mii_info_dat.open_file(gendb + "/movie_info_idx/info.dat");

        m_mc_movie_off.open_file(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
        m_mc_company_id.open_file(gendb + "/movie_companies/company_id.bin");

        m_cn_country.open_file(gendb + "/company_name/country_code.bin");
        m_cn_name_off.open_file(gendb + "/company_name/name.off");
        m_cn_name_dat.open_file(gendb + "/company_name/name.dat");
        m_cn_dict_off.open_file(gendb + "/company_name/country_code.dict.off");
        m_cn_dict_dat.open_file(gendb + "/company_name/country_code.dict.dat");

        m_kt_kind_off.open_file(gendb + "/kind_type/kind.off");
        m_kt_kind_dat.open_file(gendb + "/kind_type/kind.dat");
        m_kt_id.open_file(gendb + "/kind_type/id.bin");

        m_lt_link_off.open_file(gendb + "/link_type/link.off");
        m_lt_link_dat.open_file(gendb + "/link_type/link.dat");
        m_lt_id.open_file(gendb + "/link_type/id.bin");

        m_it_info_off.open_file(gendb + "/info_type/info.off");
        m_it_info_dat.open_file(gendb + "/info_type/info.dat");
        m_it_id.open_file(gendb + "/info_type/id.bin");
    }

    // --------------------------------- resolve literals ---------------------------------
    int32_t it_rating_id = -1;
    {
        size_t n = m_it_id.size / sizeof(int32_t);
        const int32_t* ids = m_it_id.as<int32_t>();
        const int64_t* off = m_it_info_off.as<int64_t>();
        const char* dat = m_it_info_dat.as<char>();
        std::string_view target(info_eq);
        for (size_t r = 0; r < n; ++r) {
            auto s = sv_from(dat, off, r);
            if (s == target) { it_rating_id = ids[r]; break; }
        }
    }
    if (it_rating_id < 0) { fprintf(stderr, "info_type 'rating' not found\n"); return 1; }

    std::array<int32_t, 4> kt_allowed{}; int kt_n = 0;
    {
        size_t n = m_kt_id.size / sizeof(int32_t);
        const int32_t* ids = m_kt_id.as<int32_t>();
        const int64_t* off = m_kt_kind_off.as<int64_t>();
        const char* dat = m_kt_kind_dat.as<char>();
        for (size_t r = 0; r < n; ++r) {
            auto s = sv_from(dat, off, r);
            if (s == "tv series" || s == "episode") {
                kt_allowed[kt_n++] = ids[r];
            }
        }
    }

    std::array<int32_t, 4> lt_allowed{}; int lt_n = 0;
    {
        size_t n = m_lt_id.size / sizeof(int32_t);
        const int32_t* ids = m_lt_id.as<int32_t>();
        const int64_t* off = m_lt_link_off.as<int64_t>();
        const char* dat = m_lt_link_dat.as<char>();
        for (size_t r = 0; r < n; ++r) {
            auto s = sv_from(dat, off, r);
            if (s == "sequel" || s == "follows" || s == "followed by") {
                lt_allowed[lt_n++] = ids[r];
            }
        }
    }

    // Resolve us_code in country_code dict
    int16_t us_code = -1;
    {
        size_t n_entries = (m_cn_dict_off.size / sizeof(int64_t)) - 1;
        const int64_t* off = m_cn_dict_off.as<int64_t>();
        const char* dat = m_cn_dict_dat.as<char>();
        std::string_view target(country_neq);
        for (size_t r = 0; r < n_entries; ++r) {
            std::string_view s(dat + off[r], off[r+1] - off[r]);
            if (s == target) { us_code = (int16_t)(r + 1); break; }  // dict code = idx+1
        }
    }
    // us_code may be -1 if not found; treat as "no row matches us → never excluded"

    // --------------------------------- main scan ---------------------------------
    const int32_t* ml_movie_id = m_ml_movie_id.as<int32_t>();
    const int32_t* ml_linked   = m_ml_linked.as<int32_t>();
    const int32_t* ml_lt       = m_ml_lt.as<int32_t>();
    size_t n_ml = m_ml_movie_id.size / sizeof(int32_t);

    const int32_t* t_kind      = m_t_kind.as<int32_t>();
    const int32_t* t_prod      = m_t_prod_year.as<int32_t>();
    const int64_t* t_title_off = m_t_title_off.as<int64_t>();
    const char*    t_title_dat = m_t_title_dat.as<char>();

    const int32_t* mii_movie_off  = m_mii_movie_off.as<int32_t>();
    const int32_t* mii_info_type  = m_mii_info_type.as<int32_t>();
    const int64_t* mii_info_off   = m_mii_info_off.as<int64_t>();
    const char*    mii_info_dat   = m_mii_info_dat.as<char>();

    const int32_t* mc_movie_off   = m_mc_movie_off.as<int32_t>();
    const int32_t* mc_company_id  = m_mc_company_id.as<int32_t>();

    const int16_t* cn_country     = m_cn_country.as<int16_t>();
    const int64_t* cn_name_off    = m_cn_name_off.as<int64_t>();
    const char*    cn_name_dat    = m_cn_name_dat.as<char>();

    std::string_view info_upper_sv(info_upper);

    // Per-thread mins
    int n_threads = (int)std::thread::hardware_concurrency();
    if (n_threads <= 0) n_threads = 1;
    if (n_threads > 12) n_threads = 12;

    struct MinSlots {
        std::string_view first_company, second_company;
        std::string_view first_rating,  second_rating;
        std::string_view first_movie,   second_movie;
        bool has = false;
    };
    std::vector<MinSlots> per_thread(n_threads);

    auto upd_min = [](std::string_view& cur, std::string_view cand, bool& has_slot) {
        if (!has_slot) { cur = cand; has_slot = true; }
        else if (sv_cmp(cand, cur) < 0) cur = cand;
    };

    {
        GENDB_PHASE("main_scan");

        std::vector<std::thread> threads;
        size_t chunk = (n_ml + n_threads - 1) / n_threads;

        for (int tid = 0; tid < n_threads; ++tid) {
            size_t start = (size_t)tid * chunk;
            size_t end   = std::min(start + chunk, n_ml);
            if (start >= end) continue;

            threads.emplace_back([&, tid, start, end]() {
                MinSlots local;
                bool fc_set=false, sc_set=false, fr_set=false, sr_set=false, fm_set=false, sm_set=false;

                for (size_t r = start; r < end; ++r) {
                    int32_t lt_id = ml_lt[r];
                    bool lt_ok = false;
                    for (int i = 0; i < lt_n; ++i) if (lt_allowed[i] == lt_id) { lt_ok = true; break; }
                    if (!lt_ok) continue;

                    int32_t t1_id = ml_movie_id[r];
                    int32_t t2_id = ml_linked[r];
                    if (t1_id <= 0 || t2_id <= 0) continue;

                    // t2 filters
                    int32_t t2_row = t2_id - 1;
                    int32_t t2_py = t_prod[t2_row];
                    if (t2_py < prod_lo || t2_py > prod_hi) continue;
                    int32_t t2_kind = t_kind[t2_row];
                    bool t2_ok = false;
                    for (int i = 0; i < kt_n; ++i) if (kt_allowed[i] == t2_kind) { t2_ok = true; break; }
                    if (!t2_ok) continue;

                    // t1 filter
                    int32_t t1_row = t1_id - 1;
                    int32_t t1_kind = t_kind[t1_row];
                    bool t1_ok = false;
                    for (int i = 0; i < kt_n; ++i) if (kt_allowed[i] == t1_kind) { t1_ok = true; break; }
                    if (!t1_ok) continue;

                    // mi_idx2 probe for t2: info_type==rating AND info<info_upper
                    std::string_view min_second_rating; bool sr_local = false;
                    {
                        uint64_t lo = (uint64_t)mii_movie_off[t2_id];
                        uint64_t hi = (uint64_t)mii_movie_off[t2_id + 1];
                        for (uint64_t k = lo; k < hi; ++k) {
                            if (mii_info_type[k] != it_rating_id) continue;
                            std::string_view s(mii_info_dat + mii_info_off[k], mii_info_off[k+1] - mii_info_off[k]);
                            if (sv_cmp(s, info_upper_sv) >= 0) continue;
                            if (!sr_local || sv_cmp(s, min_second_rating) < 0) {
                                min_second_rating = s; sr_local = true;
                            }
                        }
                    }
                    if (!sr_local) continue;

                    // mi_idx1 probe for t1: info_type==rating
                    std::string_view min_first_rating; bool fr_local = false;
                    {
                        uint64_t lo = (uint64_t)mii_movie_off[t1_id];
                        uint64_t hi = (uint64_t)mii_movie_off[t1_id + 1];
                        for (uint64_t k = lo; k < hi; ++k) {
                            if (mii_info_type[k] != it_rating_id) continue;
                            std::string_view s(mii_info_dat + mii_info_off[k], mii_info_off[k+1] - mii_info_off[k]);
                            if (!fr_local || sv_cmp(s, min_first_rating) < 0) {
                                min_first_rating = s; fr_local = true;
                            }
                        }
                    }
                    if (!fr_local) continue;

                    // mc1 probe for t1: cn1.country_code != us_code; track min(cn1.name)
                    std::string_view min_first_company; bool fc_local = false;
                    {
                        uint64_t lo = (uint64_t)mc_movie_off[t1_id];
                        uint64_t hi = (uint64_t)mc_movie_off[t1_id + 1];
                        for (uint64_t k = lo; k < hi; ++k) {
                            int32_t cn_id = mc_company_id[k];
                            if (cn_id <= 0) continue;
                            int32_t cn_row = cn_id - 1;
                            int16_t cc = cn_country[cn_row];
                            if (cc == us_code) continue;  // exclude [us]; null/code-0 always passes
                            std::string_view s(cn_name_dat + cn_name_off[cn_row], cn_name_off[cn_row+1] - cn_name_off[cn_row]);
                            if (!fc_local || sv_cmp(s, min_first_company) < 0) {
                                min_first_company = s; fc_local = true;
                            }
                        }
                    }
                    if (!fc_local) continue;

                    // mc2 probe for t2: any row; track min(cn2.name)
                    std::string_view min_second_company; bool sc_local = false;
                    {
                        uint64_t lo = (uint64_t)mc_movie_off[t2_id];
                        uint64_t hi = (uint64_t)mc_movie_off[t2_id + 1];
                        for (uint64_t k = lo; k < hi; ++k) {
                            int32_t cn_id = mc_company_id[k];
                            if (cn_id <= 0) continue;
                            int32_t cn_row = cn_id - 1;
                            std::string_view s(cn_name_dat + cn_name_off[cn_row], cn_name_off[cn_row+1] - cn_name_off[cn_row]);
                            if (!sc_local || sv_cmp(s, min_second_company) < 0) {
                                min_second_company = s; sc_local = true;
                            }
                        }
                    }
                    if (!sc_local) continue;

                    // titles
                    std::string_view t1_title(t_title_dat + t_title_off[t1_row], t_title_off[t1_row+1] - t_title_off[t1_row]);
                    std::string_view t2_title(t_title_dat + t_title_off[t2_row], t_title_off[t2_row+1] - t_title_off[t2_row]);

                    // update local mins
                    upd_min(local.first_company,  min_first_company,  fc_set);
                    upd_min(local.second_company, min_second_company, sc_set);
                    upd_min(local.first_rating,   min_first_rating,   fr_set);
                    upd_min(local.second_rating,  min_second_rating,  sr_set);
                    upd_min(local.first_movie,    t1_title,           fm_set);
                    upd_min(local.second_movie,   t2_title,           sm_set);
                    local.has = true;
                }
                per_thread[tid] = local;
            });
        }

        for (auto& th : threads) th.join();
    }

    // Reduce
    MinSlots out;
    bool fc=false, sc=false, fr=false, sr=false, fm=false, sm=false;
    for (auto& m : per_thread) {
        if (!m.has) continue;
        upd_min(out.first_company,  m.first_company,  fc);
        upd_min(out.second_company, m.second_company, sc);
        upd_min(out.first_rating,   m.first_rating,   fr);
        upd_min(out.second_rating,  m.second_rating,  sr);
        upd_min(out.first_movie,    m.first_movie,    fm);
        upd_min(out.second_movie,   m.second_movie,   sm);
        out.has = true;
    }

    // --------------------------------- output ---------------------------------
    {
        GENDB_PHASE("output");
        // mkdir -p results_dir
        ::mkdir(results_dir.c_str(), 0755);
        std::string path = results_dir + "/Q33c.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { fprintf(stderr, "cannot open output %s\n", path.c_str()); return 1; }
        std::fprintf(f, "first_company,second_company,first_rating,second_rating,first_movie,second_movie\n");
        if (out.has) {
            std::fwrite(out.first_company.data(), 1, out.first_company.size(), f);  std::fputc(',', f);
            std::fwrite(out.second_company.data(), 1, out.second_company.size(), f); std::fputc(',', f);
            std::fwrite(out.first_rating.data(), 1, out.first_rating.size(), f);     std::fputc(',', f);
            std::fwrite(out.second_rating.data(), 1, out.second_rating.size(), f);   std::fputc(',', f);
            std::fwrite(out.first_movie.data(), 1, out.first_movie.size(), f);       std::fputc(',', f);
            std::fwrite(out.second_movie.data(), 1, out.second_movie.size(), f);     std::fputc('\n', f);
        } else {
            std::fprintf(f, ",,,,,\n");
        }
        std::fclose(f);
    }

    return 0;
}
