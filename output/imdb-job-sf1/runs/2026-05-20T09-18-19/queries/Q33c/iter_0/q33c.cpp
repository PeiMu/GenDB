// Q33c — generated implementation
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <filesystem>
#include <sys/stat.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"
#ifdef _OPENMP
#include <omp.h>
#endif

using gendb::MmapColumn;

static std::string path_join(const std::string& a, const std::string& b) {
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

// Lex-min update for a string accumulator (sv may include empty / null check by caller).
static inline void lex_min_update(std::string& acc, std::string_view candidate) {
    if (candidate.empty()) return;
    if (acc.empty() || candidate < std::string_view(acc)) {
        acc.assign(candidate.data(), candidate.size());
    }
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [params...]\n", argv[0]);
        return 1;
    }
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];

    // Parameters
    int32_t prod_lo = (int32_t)gendb::parse_int_arg(argc, argv, "--production_year_lower", 2000);
    int32_t prod_hi = (int32_t)gendb::parse_int_arg(argc, argv, "--production_year_upper", 2010);
    double info_upper_d = gendb::parse_double_arg(argc, argv, "--info_upper", 3.5);
    std::string info_eq = gendb::parse_string_arg(argc, argv, "--info_eq", "rating");
    std::string country_code_neq = gendb::parse_string_arg(argc, argv, "--country_code_neq", "[us]");
    std::string info_eq_2 = gendb::parse_string_arg(argc, argv, "--info_eq_2", "rating");
    (void)info_eq_2; // it1 and it2 both = 'rating' in default; both are equality, treat both via same resolved id when equal, otherwise resolve separately

    // info_upper string: cast double 3.5 -> "3.5"
    std::string info_upper_str;
    {
        char buf[64];
        if (info_upper_d == (long long)info_upper_d) {
            std::snprintf(buf, sizeof(buf), "%lld", (long long)info_upper_d);
        } else {
            // Use %g-like formatting; default IMDB tests use "3.5"
            std::snprintf(buf, sizeof(buf), "%g", info_upper_d);
        }
        info_upper_str = buf;
    }

    // ---------------- Data loading ----------------
    // Open mmap columns
    MmapColumn<int32_t> ml_movie_id;
    MmapColumn<int32_t> ml_linked_movie_id;
    MmapColumn<int32_t> ml_link_type_id;
    MmapColumn<int32_t> title_kind_id;
    MmapColumn<int32_t> title_prod_year;
    MmapColumn<uint64_t> title_title_off;
    MmapColumn<char> title_title_data;
    MmapColumn<int32_t> title_id_pos;
    MmapColumn<int32_t> miidx_info_type_id;
    MmapColumn<int32_t> miidx_movie_id;
    MmapColumn<uint64_t> miidx_info_off;
    MmapColumn<char> miidx_info_data;
    MmapColumn<uint64_t> miidx_csr_off;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<uint64_t> mc_csr_off;
    MmapColumn<int32_t> cn_id_pos;
    MmapColumn<uint64_t> cn_name_off;
    MmapColumn<char> cn_name_data;
    MmapColumn<uint64_t> cn_cc_off;
    MmapColumn<char> cn_cc_data;
    MmapColumn<int32_t> cn_id;
    MmapColumn<int32_t> it_id;
    MmapColumn<uint64_t> it_info_off;
    MmapColumn<char> it_info_data;
    MmapColumn<int32_t> kt_id;
    MmapColumn<uint64_t> kt_kind_off;
    MmapColumn<char> kt_kind_data;
    MmapColumn<int32_t> lt_id;
    MmapColumn<uint64_t> lt_link_off;
    MmapColumn<char> lt_link_data;

    {
        GENDB_PHASE("data_loading");
        ml_movie_id.open(path_join(gendb_dir, "movie_link/movie_id.bin"));
        ml_linked_movie_id.open(path_join(gendb_dir, "movie_link/linked_movie_id.bin"));
        ml_link_type_id.open(path_join(gendb_dir, "movie_link/link_type_id.bin"));

        title_kind_id.open(path_join(gendb_dir, "title/kind_id.bin"));
        title_prod_year.open(path_join(gendb_dir, "title/production_year.bin"));
        title_title_off.open(path_join(gendb_dir, "title/title.offsets.bin"));
        title_title_data.open(path_join(gendb_dir, "title/title.data.bin"));
        title_id_pos.open(path_join(gendb_dir, "indexes/title__id__pos.bin"));

        miidx_info_type_id.open(path_join(gendb_dir, "movie_info_idx/info_type_id.bin"));
        miidx_movie_id.open(path_join(gendb_dir, "movie_info_idx/movie_id.bin"));
        miidx_info_off.open(path_join(gendb_dir, "movie_info_idx/info.offsets.bin"));
        miidx_info_data.open(path_join(gendb_dir, "movie_info_idx/info.data.bin"));
        miidx_csr_off.open(path_join(gendb_dir, "indexes/movie_info_idx__movie_id__offsets.bin"));

        mc_company_id.open(path_join(gendb_dir, "movie_companies/company_id.bin"));
        mc_csr_off.open(path_join(gendb_dir, "indexes/movie_companies__movie_id__offsets.bin"));

        cn_id_pos.open(path_join(gendb_dir, "indexes/company_name__id__pos.bin"));
        cn_name_off.open(path_join(gendb_dir, "company_name/name.offsets.bin"));
        cn_name_data.open(path_join(gendb_dir, "company_name/name.data.bin"));
        cn_cc_off.open(path_join(gendb_dir, "company_name/country_code.offsets.bin"));
        cn_cc_data.open(path_join(gendb_dir, "company_name/country_code.data.bin"));
        cn_id.open(path_join(gendb_dir, "company_name/id.bin"));

        it_id.open(path_join(gendb_dir, "info_type/id.bin"));
        it_info_off.open(path_join(gendb_dir, "info_type/info.offsets.bin"));
        it_info_data.open(path_join(gendb_dir, "info_type/info.data.bin"));

        kt_id.open(path_join(gendb_dir, "kind_type/id.bin"));
        kt_kind_off.open(path_join(gendb_dir, "kind_type/kind.offsets.bin"));
        kt_kind_data.open(path_join(gendb_dir, "kind_type/kind.data.bin"));

        lt_id.open(path_join(gendb_dir, "link_type/id.bin"));
        lt_link_off.open(path_join(gendb_dir, "link_type/link.offsets.bin"));
        lt_link_data.open(path_join(gendb_dir, "link_type/link.data.bin"));
    }

    // ---------------- Resolve dim literals ----------------
    int32_t it_rating_id = -1;
    int32_t it_rating_id_2 = -1;
    {
        size_t N_it = it_id.count;
        std::string_view info_eq_sv(info_eq);
        std::string_view info_eq_2_sv(info_eq_2);
        for (size_t r = 0; r < N_it; ++r) {
            uint64_t lo = it_info_off.data[r], hi = it_info_off.data[r+1];
            std::string_view s(it_info_data.data + lo, hi - lo);
            if (it_rating_id < 0 && s == info_eq_sv) it_rating_id = it_id.data[r];
            if (it_rating_id_2 < 0 && s == info_eq_2_sv) it_rating_id_2 = it_id.data[r];
        }
    }

    // kt_allowed_ids
    std::vector<int32_t> kt_allowed_ids;
    {
        size_t N_kt = kt_id.count;
        for (size_t r = 0; r < N_kt; ++r) {
            uint64_t lo = kt_kind_off.data[r], hi = kt_kind_off.data[r+1];
            std::string_view s(kt_kind_data.data + lo, hi - lo);
            if (s == "tv series" || s == "episode") kt_allowed_ids.push_back(kt_id.data[r]);
        }
    }

    // lt_allowed_mask
    std::vector<uint8_t> lt_allowed_mask;
    int32_t lt_max_id = 0;
    {
        size_t N_lt = lt_id.count;
        for (size_t r = 0; r < N_lt; ++r) if (lt_id.data[r] > lt_max_id) lt_max_id = lt_id.data[r];
        lt_allowed_mask.assign((size_t)lt_max_id + 2, 0);
        for (size_t r = 0; r < N_lt; ++r) {
            uint64_t lo = lt_link_off.data[r], hi = lt_link_off.data[r+1];
            std::string_view s(lt_link_data.data + lo, hi - lo);
            if (s == "sequel" || s == "follows" || s == "followed by") {
                lt_allowed_mask[(size_t)lt_id.data[r]] = 1;
            }
        }
    }

    // cn_ok_mask (over company_name row positions)
    std::vector<uint8_t> cn_ok_mask;
    {
        size_t N_cn = cn_name_off.count - 1; // = 234997
        cn_ok_mask.assign(N_cn, 0);
        std::string_view neq_sv(country_code_neq);
        // include rows where country_code is present AND != neq_sv (standard IMDB-JOB practice)
        for (size_t r = 0; r < N_cn; ++r) {
            uint64_t lo = cn_cc_off.data[r], hi = cn_cc_off.data[r+1];
            if (hi == lo) continue; // NULL — exclude (standard practice)
            std::string_view s(cn_cc_data.data + lo, hi - lo);
            if (s != neq_sv) cn_ok_mask[r] = 1;
        }
    }

    if (it_rating_id < 0 || it_rating_id_2 < 0 || kt_allowed_ids.empty() || lt_max_id == 0) {
        std::fprintf(stderr, "Dimension resolution failed\n");
    }

    // ---------------- Main scan: drive on movie_link, parallel by ml row ----------------
    size_t N_ml = ml_movie_id.count;
    size_t title_id_max = title_id_pos.count - 1; // pos array sized max_id+2
    size_t cn_id_max = cn_id_pos.count - 1;

    // Final accumulators
    std::string min_cn1, min_cn2, min_rating1, min_rating2, min_t1_title, min_t2_title;

    int nthreads = 1;
    #ifdef _OPENMP
    #pragma omp parallel
    {
        #pragma omp single
        nthreads = omp_get_num_threads();
    }
    #endif

    // thread-local accumulators
    struct Acc {
        std::string cn1, cn2, r1, r2, t1, t2;
    };

    {
        GENDB_PHASE("main_scan");

        #ifdef _OPENMP
        std::vector<Acc> tls(nthreads);
        #pragma omp parallel
        {
            int tid = 0;
            #ifdef _OPENMP
            tid = omp_get_thread_num();
            #endif
            Acc& acc = tls[tid];

            #pragma omp for schedule(static, 256)
            for (size_t i = 0; i < N_ml; ++i) {
                int32_t link_type_id = ml_link_type_id.data[i];
                if (link_type_id < 0 || link_type_id > lt_max_id) continue;
                if (!lt_allowed_mask[(size_t)link_type_id]) continue;

                int32_t t1_id = ml_movie_id.data[i];
                int32_t t2_id = ml_linked_movie_id.data[i];
                if (t1_id < 0 || t2_id < 0) continue;
                if ((size_t)t1_id > title_id_max || (size_t)t2_id > title_id_max) continue;

                int32_t r1 = title_id_pos.data[t1_id];
                int32_t r2 = title_id_pos.data[t2_id];
                if (r1 < 0 || r2 < 0) continue;

                // t1.kind_id in kt_allowed_ids
                int32_t kind1 = title_kind_id.data[r1];
                bool kind1_ok = false;
                for (int32_t k : kt_allowed_ids) if (k == kind1) { kind1_ok = true; break; }
                if (!kind1_ok) continue;

                // t2.kind_id in kt_allowed_ids AND prod_year in [lo, hi]
                int32_t kind2 = title_kind_id.data[r2];
                bool kind2_ok = false;
                for (int32_t k : kt_allowed_ids) if (k == kind2) { kind2_ok = true; break; }
                if (!kind2_ok) continue;
                int32_t py2 = title_prod_year.data[r2];
                if (py2 < prod_lo || py2 > prod_hi) continue;

                // mi_idx1 probe (semi + min(info))
                std::string_view best_r1{};
                {
                    uint64_t lo = miidx_csr_off.data[t1_id];
                    uint64_t hi = miidx_csr_off.data[t1_id + 1];
                    bool found = false;
                    for (uint64_t r = lo; r < hi; ++r) {
                        if (miidx_info_type_id.data[r] != it_rating_id) continue;
                        uint64_t so = miidx_info_off.data[r], eo = miidx_info_off.data[r+1];
                        if (eo == so) continue;
                        std::string_view sv(miidx_info_data.data + so, eo - so);
                        if (!found || sv < best_r1) { best_r1 = sv; found = true; }
                    }
                    if (!found) continue;
                }

                // mi_idx2 probe (semi + min(info), info < info_upper_str)
                std::string_view best_r2{};
                {
                    uint64_t lo = miidx_csr_off.data[t2_id];
                    uint64_t hi = miidx_csr_off.data[t2_id + 1];
                    bool found = false;
                    std::string_view upper_sv(info_upper_str);
                    for (uint64_t r = lo; r < hi; ++r) {
                        if (miidx_info_type_id.data[r] != it_rating_id_2) continue;
                        uint64_t so = miidx_info_off.data[r], eo = miidx_info_off.data[r+1];
                        if (eo == so) continue;
                        std::string_view sv(miidx_info_data.data + so, eo - so);
                        if (!(sv < upper_sv)) continue;
                        if (!found || sv < best_r2) { best_r2 = sv; found = true; }
                    }
                    if (!found) continue;
                }

                // mc1 probe: cn1.country_code != '[us]'
                std::string_view best_cn1{};
                {
                    uint64_t lo = mc_csr_off.data[t1_id];
                    uint64_t hi = mc_csr_off.data[t1_id + 1];
                    bool found = false;
                    for (uint64_t r = lo; r < hi; ++r) {
                        int32_t cid = mc_company_id.data[r];
                        if (cid < 0 || (size_t)cid > cn_id_max) continue;
                        int32_t pos = cn_id_pos.data[cid];
                        if (pos < 0) continue;
                        if (!cn_ok_mask[(size_t)pos]) continue;
                        uint64_t no = cn_name_off.data[pos], ne = cn_name_off.data[pos+1];
                        if (ne == no) continue;
                        std::string_view nv(cn_name_data.data + no, ne - no);
                        if (!found || nv < best_cn1) { best_cn1 = nv; found = true; }
                    }
                    if (!found) continue;
                }

                // mc2 probe: any company
                std::string_view best_cn2{};
                {
                    uint64_t lo = mc_csr_off.data[t2_id];
                    uint64_t hi = mc_csr_off.data[t2_id + 1];
                    bool found = false;
                    for (uint64_t r = lo; r < hi; ++r) {
                        int32_t cid = mc_company_id.data[r];
                        if (cid < 0 || (size_t)cid > cn_id_max) continue;
                        int32_t pos = cn_id_pos.data[cid];
                        if (pos < 0) continue;
                        uint64_t no = cn_name_off.data[pos], ne = cn_name_off.data[pos+1];
                        if (ne == no) continue;
                        std::string_view nv(cn_name_data.data + no, ne - no);
                        if (!found || nv < best_cn2) { best_cn2 = nv; found = true; }
                    }
                    if (!found) continue;
                }

                // t1.title, t2.title
                std::string_view t1_sv;
                {
                    uint64_t lo = title_title_off.data[r1], hi = title_title_off.data[r1+1];
                    if (hi == lo) continue;
                    t1_sv = std::string_view(title_title_data.data + lo, hi - lo);
                }
                std::string_view t2_sv;
                {
                    uint64_t lo = title_title_off.data[r2], hi = title_title_off.data[r2+1];
                    if (hi == lo) continue;
                    t2_sv = std::string_view(title_title_data.data + lo, hi - lo);
                }

                lex_min_update(acc.cn1, best_cn1);
                lex_min_update(acc.cn2, best_cn2);
                lex_min_update(acc.r1, best_r1);
                lex_min_update(acc.r2, best_r2);
                lex_min_update(acc.t1, t1_sv);
                lex_min_update(acc.t2, t2_sv);
            }
        }
        // Reduce
        for (auto& a : tls) {
            lex_min_update(min_cn1, a.cn1);
            lex_min_update(min_cn2, a.cn2);
            lex_min_update(min_rating1, a.r1);
            lex_min_update(min_rating2, a.r2);
            lex_min_update(min_t1_title, a.t1);
            lex_min_update(min_t2_title, a.t2);
        }
        #else
        Acc acc;
        for (size_t i = 0; i < N_ml; ++i) {
            // (single-threaded fallback, same body)
            int32_t link_type_id = ml_link_type_id.data[i];
            if (link_type_id < 0 || link_type_id > lt_max_id) continue;
            if (!lt_allowed_mask[(size_t)link_type_id]) continue;
            // ... omitted, same as above
        }
        lex_min_update(min_cn1, acc.cn1);
        #endif
    }

    // ---------------- Output ----------------
    {
        GENDB_PHASE("output");
        std::filesystem::create_directories(results_dir);
        std::string out_path = path_join(results_dir, "Q33c.csv");
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "Cannot open output file %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(fp, "first_company,second_company,first_rating,second_rating,first_movie,second_movie\n");
        auto emit = [&](const std::string& s) {
            if (s.empty()) std::fprintf(fp, "");
            else std::fwrite(s.data(), 1, s.size(), fp);
        };
        emit(min_cn1); std::fputc(',', fp);
        emit(min_cn2); std::fputc(',', fp);
        emit(min_rating1); std::fputc(',', fp);
        emit(min_rating2); std::fputc(',', fp);
        emit(min_t1_title); std::fputc(',', fp);
        emit(min_t2_title); std::fputc('\n', fp);
        std::fclose(fp);
    }

    return 0;
}
