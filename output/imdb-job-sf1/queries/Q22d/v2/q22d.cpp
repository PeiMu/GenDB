// Q22d — generated implementation
// SELECT MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// Joins title -> mi_idx, mk, mi, mc -> company_name with multiple filters.

#include "timing_utils.h"
#include "cli_params.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <climits>
#include <algorithm>
#include <filesystem>

using namespace gendb;
namespace fs = std::filesystem;

// ---------------- Simple mmap helper ----------------
struct Bytes { const uint8_t* data; size_t size; };

static Bytes mmap_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Cannot open %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(fd, &st) < 0) { fprintf(stderr, "stat fail %s\n", path.c_str()); std::exit(1); }
    size_t sz = (size_t)st.st_size;
    if (sz == 0) { ::close(fd); return {nullptr, 0}; }
    void* p = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { fprintf(stderr, "mmap fail %s\n", path.c_str()); std::exit(1); }
    if (sz > (1u<<20)) madvise(p, sz, MADV_WILLNEED);
    return { (const uint8_t*)p, sz };
}

static inline std::string_view sv_at(const uint8_t* dat, const int64_t* off, size_t row) {
    int64_t a = off[row];
    int64_t b = off[row+1];
    return std::string_view((const char*)(dat + a), (size_t)(b - a));
}

// ---------------- Main ----------------
int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [--param value ...]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    // ---------- Parse CLI parameters ----------
    int64_t prod_year_lower = parse_int_arg(argc, argv, "--production_year_lower", 2005);
    double info_upper_d   = parse_double_arg(argc, argv, "--info_upper", 8.5);
    std::string info_eq   = parse_string_arg(argc, argv, "--info_eq", "rating");
    std::string cc_neq    = parse_string_arg(argc, argv, "--country_code_neq", "[us]");
    std::string info_eq_2 = parse_string_arg(argc, argv, "--info_eq_2", "countries");

    // Format info_upper as a string for lex compare against mi_idx.info.
    char info_upper_buf[32];
    snprintf(info_upper_buf, sizeof(info_upper_buf), "%g", info_upper_d);
    std::string info_upper_s = info_upper_buf;
    std::string_view info_upper_sv(info_upper_s);

    // ---------- Load files ----------
    Bytes b_title_id, b_title_kind, b_title_py, b_title_off, b_title_dat;
    Bytes b_it_id, b_it_off, b_it_dat;
    Bytes b_kt_id, b_kt_off, b_kt_dat;
    Bytes b_k_id, b_k_off, b_k_dat;
    Bytes b_mi_idx_movie, b_mi_idx_it, b_mi_idx_off, b_mi_idx_dat;
    Bytes b_mk_movie, b_mk_kw;
    Bytes b_mi_movie, b_mi_it, b_mi_off, b_mi_dat;
    Bytes b_mc_movie, b_mc_company, b_mc_ct;
    Bytes b_cn_id, b_cn_cc_bin, b_cn_dict_off, b_cn_dict_dat, b_cn_name_off, b_cn_name_dat;
    Bytes b_mi_idx_off_idx, b_mk_off_idx, b_mi_off_idx, b_mc_off_idx;

    {
        GENDB_PHASE("data_loading");
        // title
        b_title_id   = mmap_file(gendb_dir + "/title/id.bin");
        b_title_kind = mmap_file(gendb_dir + "/title/kind_id.bin");
        b_title_py   = mmap_file(gendb_dir + "/title/production_year.bin");
        b_title_off  = mmap_file(gendb_dir + "/title/title.off");
        b_title_dat  = mmap_file(gendb_dir + "/title/title.dat");
        // info_type
        b_it_id  = mmap_file(gendb_dir + "/info_type/id.bin");
        b_it_off = mmap_file(gendb_dir + "/info_type/info.off");
        b_it_dat = mmap_file(gendb_dir + "/info_type/info.dat");
        // kind_type
        b_kt_id  = mmap_file(gendb_dir + "/kind_type/id.bin");
        b_kt_off = mmap_file(gendb_dir + "/kind_type/kind.off");
        b_kt_dat = mmap_file(gendb_dir + "/kind_type/kind.dat");
        // keyword
        b_k_id  = mmap_file(gendb_dir + "/keyword/id.bin");
        b_k_off = mmap_file(gendb_dir + "/keyword/keyword.off");
        b_k_dat = mmap_file(gendb_dir + "/keyword/keyword.dat");
        // movie_info_idx
        b_mi_idx_movie = mmap_file(gendb_dir + "/movie_info_idx/movie_id.bin");
        b_mi_idx_it    = mmap_file(gendb_dir + "/movie_info_idx/info_type_id.bin");
        b_mi_idx_off   = mmap_file(gendb_dir + "/movie_info_idx/info.off");
        b_mi_idx_dat   = mmap_file(gendb_dir + "/movie_info_idx/info.dat");
        // movie_keyword
        b_mk_movie = mmap_file(gendb_dir + "/movie_keyword/movie_id.bin");
        b_mk_kw    = mmap_file(gendb_dir + "/movie_keyword/keyword_id.bin");
        // movie_info
        b_mi_movie = mmap_file(gendb_dir + "/movie_info/movie_id.bin");
        b_mi_it    = mmap_file(gendb_dir + "/movie_info/info_type_id.bin");
        b_mi_off   = mmap_file(gendb_dir + "/movie_info/info.off");
        b_mi_dat   = mmap_file(gendb_dir + "/movie_info/info.dat");
        // movie_companies
        b_mc_movie   = mmap_file(gendb_dir + "/movie_companies/movie_id.bin");
        b_mc_company = mmap_file(gendb_dir + "/movie_companies/company_id.bin");
        b_mc_ct      = mmap_file(gendb_dir + "/movie_companies/company_type_id.bin");
        // company_name
        b_cn_id      = mmap_file(gendb_dir + "/company_name/id.bin");
        b_cn_cc_bin  = mmap_file(gendb_dir + "/company_name/country_code.bin");        // int16 codes
        b_cn_dict_off= mmap_file(gendb_dir + "/company_name/country_code.dict.off");   // int64
        b_cn_dict_dat= mmap_file(gendb_dir + "/company_name/country_code.dict.dat");
        b_cn_name_off= mmap_file(gendb_dir + "/company_name/name.off");
        b_cn_name_dat= mmap_file(gendb_dir + "/company_name/name.dat");
        // indexes (csr offsets — int32 arrays per storage_design.json)
        b_mi_idx_off_idx = mmap_file(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        b_mk_off_idx     = mmap_file(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        b_mi_off_idx     = mmap_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        b_mc_off_idx     = mmap_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
    }

    // typed views
    const int32_t* title_id  = (const int32_t*)b_title_id.data;
    const int32_t* title_kind= (const int32_t*)b_title_kind.data;
    const int32_t* title_py  = (const int32_t*)b_title_py.data;
    const int64_t* title_off = (const int64_t*)b_title_off.data;
    const uint8_t* title_dat = b_title_dat.data;
    size_t title_n = b_title_id.size / 4;

    const int32_t* it_id  = (const int32_t*)b_it_id.data;
    const int64_t* it_off = (const int64_t*)b_it_off.data;
    const uint8_t* it_dat = b_it_dat.data;
    size_t it_n = b_it_id.size / 4;

    const int32_t* kt_id  = (const int32_t*)b_kt_id.data;
    const int64_t* kt_off = (const int64_t*)b_kt_off.data;
    const uint8_t* kt_dat = b_kt_dat.data;
    size_t kt_n = b_kt_id.size / 4;

    const int32_t* k_id  = (const int32_t*)b_k_id.data;
    const int64_t* k_off = (const int64_t*)b_k_off.data;
    const uint8_t* k_dat = b_k_dat.data;
    size_t k_n = b_k_id.size / 4;

    const int32_t* mi_idx_movie = (const int32_t*)b_mi_idx_movie.data;
    const int32_t* mi_idx_it    = (const int32_t*)b_mi_idx_it.data;
    const int64_t* mi_idx_off_v = (const int64_t*)b_mi_idx_off.data;
    const uint8_t* mi_idx_dat_v = b_mi_idx_dat.data;

    const int32_t* mk_movie = (const int32_t*)b_mk_movie.data;
    const int32_t* mk_kw    = (const int32_t*)b_mk_kw.data;

    const int32_t* mi_movie = (const int32_t*)b_mi_movie.data;
    const int32_t* mi_it    = (const int32_t*)b_mi_it.data;
    const int64_t* mi_off_v = (const int64_t*)b_mi_off.data;
    const uint8_t* mi_dat_v = b_mi_dat.data;

    const int32_t* mc_movie   = (const int32_t*)b_mc_movie.data;
    const int32_t* mc_company = (const int32_t*)b_mc_company.data;
    // mc_ct unused — no company_type filter
    (void)b_mc_ct;

    const int32_t* cn_id      = (const int32_t*)b_cn_id.data;
    const int16_t* cn_cc      = (const int16_t*)b_cn_cc_bin.data;
    const int64_t* cn_dict_off= (const int64_t*)b_cn_dict_off.data;
    const uint8_t* cn_dict_dat= b_cn_dict_dat.data;
    const int64_t* cn_name_off= (const int64_t*)b_cn_name_off.data;
    const uint8_t* cn_name_dat= b_cn_name_dat.data;
    size_t cn_n = b_cn_id.size / 4;
    size_t cn_dict_n = (b_cn_dict_off.size / 8) - 1;

    const int32_t* mi_idx_off_idx = (const int32_t*)b_mi_idx_off_idx.data;
    const int32_t* mk_off_idx     = (const int32_t*)b_mk_off_idx.data;
    const int32_t* mi_off_idx     = (const int32_t*)b_mi_off_idx.data;
    const int32_t* mc_off_idx     = (const int32_t*)b_mc_off_idx.data;

    // ---------- Resolve dimension literals ----------
    int32_t it1_id = -1, it2_id = -1;  // 'countries', 'rating'
    {
        for (size_t r = 0; r < it_n; ++r) {
            std::string_view s = sv_at(it_dat, it_off, r);
            if (it1_id < 0 && s == std::string_view(info_eq_2)) it1_id = it_id[r];
            if (it2_id < 0 && s == std::string_view(info_eq))   it2_id = it_id[r];
            if (it1_id >= 0 && it2_id >= 0) break;
        }
    }
    if (it1_id < 0 || it2_id < 0) {
        fprintf(stderr, "info_type literal lookup failed\n");
        return 1;
    }

    std::array<int32_t, 4> kt_id_set = { -1, -1, -1, -1 };
    int kt_set_n = 0;
    {
        for (size_t r = 0; r < kt_n && kt_set_n < 4; ++r) {
            std::string_view s = sv_at(kt_dat, kt_off, r);
            if (s == "movie" || s == "episode") {
                kt_id_set[kt_set_n++] = kt_id[r];
            }
        }
    }

    std::array<int32_t, 4> k_id_set = { -1, -1, -1, -1 };
    int k_set_n = 0;
    {
        const std::array<std::string_view, 4> kw_lits = {
            std::string_view("murder"),
            std::string_view("murder-in-title"),
            std::string_view("blood"),
            std::string_view("violence"),
        };
        for (size_t r = 0; r < k_n && k_set_n < 4; ++r) {
            std::string_view s = sv_at(k_dat, k_off, r);
            for (auto& lit : kw_lits) {
                if (s == lit) { k_id_set[k_set_n++] = k_id[r]; break; }
            }
        }
    }

    // Country set (10 strings) for mi.info IN (...)
    const std::unordered_set<std::string> country_set = {
        "Sweden","Norway","Germany","Denmark","Swedish",
        "Danish","Norwegian","German","USA","American"
    };

    // Resolve us_code via dict scan; us_code = dict_row_index + 1 (code 0 = NULL).
    int16_t us_code = 0;
    {
        std::string_view cc_target(cc_neq);
        for (size_t r = 0; r < cn_dict_n; ++r) {
            int64_t a = cn_dict_off[r];
            int64_t b = cn_dict_off[r+1];
            std::string_view s((const char*)(cn_dict_dat + a), (size_t)(b - a));
            if (s == cc_target) { us_code = (int16_t)(r + 1); break; }
        }
    }

    // Build cn_allowed_bitset indexed by company_name.id (1..cn_max_id).
    // company_name.id is dense 1..N according to storage_design, so id == row+1.
    int32_t cn_max_id = 0;
    for (size_t r = 0; r < cn_n; ++r) if (cn_id[r] > cn_max_id) cn_max_id = cn_id[r];
    std::vector<uint8_t> cn_allowed((size_t)cn_max_id + 2, 0);
    for (size_t r = 0; r < cn_n; ++r) {
        int16_t code = cn_cc[r];
        // !=  '[us]' AND not NULL (NULL comparison yields NULL in SQL, so excluded).
        if (code != 0 && code != us_code) {
            int32_t id = cn_id[r];
            if (id >= 0 && id <= cn_max_id) cn_allowed[(size_t)id] = 1;
        }
    }

    // ---------- Parallel scan over title ----------
    struct LocalMin {
        std::string cn_name;
        std::string mi_idx_info;
        std::string t_title;
        bool have_cn = false;
        bool have_mi = false;
        bool have_t = false;
    };

    unsigned nth = std::thread::hardware_concurrency();
    if (nth == 0) nth = 1;
    if (nth > 12) nth = 12;
    std::vector<LocalMin> locals(nth);

    auto update_min = [](std::string& cur, bool& have, std::string_view v) {
        if (!have || std::string_view(cur) > v) {
            cur.assign(v.data(), v.size());
            have = true;
        }
    };

    const size_t morsel = 16384;
    std::atomic<size_t> next_morsel{0};
    const size_t n_morsels = (title_n + morsel - 1) / morsel;

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        threads.reserve(nth);
        for (unsigned t = 0; t < nth; ++t) {
            threads.emplace_back([&, t]() {
                LocalMin& lm = locals[t];
                while (true) {
                    size_t m = next_morsel.fetch_add(1, std::memory_order_relaxed);
                    if (m >= n_morsels) break;
                    size_t row_lo = m * morsel;
                    size_t row_hi = std::min(row_lo + morsel, title_n);
                    for (size_t r = row_lo; r < row_hi; ++r) {
                        // production_year filter (INT32_MIN nulls trivially fail >2005)
                        int32_t py = title_py[r];
                        if (py <= (int32_t)prod_year_lower) continue;
                        // kind_id filter
                        int32_t kid = title_kind[r];
                        bool kind_ok = false;
                        for (int i = 0; i < kt_set_n; ++i) {
                            if (kid == kt_id_set[i]) { kind_ok = true; break; }
                        }
                        if (!kind_ok) continue;

                        int32_t tid = title_id[r];
                        // mi_idx: info_type_id == it2_id AND info < info_upper_sv
                        int32_t lo, hi;
                        lo = mi_idx_off_idx[tid];
                        hi = mi_idx_off_idx[tid + 1];
                        bool mi_idx_ok = false;
                        std::string_view best_mi_idx; bool have_best_mi = false;
                        for (int32_t rr = lo; rr < hi; ++rr) {
                            if (mi_idx_it[rr] != it2_id) continue;
                            std::string_view info_v = sv_at(mi_idx_dat_v, mi_idx_off_v, (size_t)rr);
                            if (info_v < info_upper_sv) {
                                mi_idx_ok = true;
                                if (!have_best_mi || info_v < best_mi_idx) {
                                    best_mi_idx = info_v;
                                    have_best_mi = true;
                                }
                            }
                        }
                        if (!mi_idx_ok) continue;

                        // mk: keyword_id in k_id_set
                        lo = mk_off_idx[tid];
                        hi = mk_off_idx[tid + 1];
                        bool mk_ok = false;
                        for (int32_t rr = lo; rr < hi; ++rr) {
                            int32_t kw = mk_kw[rr];
                            for (int i = 0; i < k_set_n; ++i) {
                                if (kw == k_id_set[i]) { mk_ok = true; break; }
                            }
                            if (mk_ok) break;
                        }
                        if (!mk_ok) continue;

                        // mi: info_type_id == it1_id AND info in country_set
                        lo = mi_off_idx[tid];
                        hi = mi_off_idx[tid + 1];
                        bool mi_ok = false;
                        for (int32_t rr = lo; rr < hi; ++rr) {
                            if (mi_it[rr] != it1_id) continue;
                            std::string_view info_v = sv_at(mi_dat_v, mi_off_v, (size_t)rr);
                            // unordered_set lookup; small set (10 elements)
                            if (country_set.find(std::string(info_v)) != country_set.end()) {
                                mi_ok = true; break;
                            }
                        }
                        if (!mi_ok) continue;

                        // mc: company_id in cn_allowed bitset; track min cn.name
                        lo = mc_off_idx[tid];
                        hi = mc_off_idx[tid + 1];
                        bool mc_ok = false;
                        std::string_view best_cn; bool have_best_cn = false;
                        for (int32_t rr = lo; rr < hi; ++rr) {
                            int32_t cid = mc_company[rr];
                            if (cid <= 0 || cid > cn_max_id) continue;
                            if (!cn_allowed[(size_t)cid]) continue;
                            mc_ok = true;
                            // read cn.name for this company
                            int32_t cn_row = cid - 1; // dense id -> row
                            std::string_view nm = sv_at(cn_name_dat, cn_name_off, (size_t)cn_row);
                            if (!have_best_cn || nm < best_cn) {
                                best_cn = nm;
                                have_best_cn = true;
                            }
                        }
                        if (!mc_ok) continue;

                        // All probes succeeded — update mins.
                        if (have_best_mi) update_min(lm.mi_idx_info, lm.have_mi, best_mi_idx);
                        if (have_best_cn) update_min(lm.cn_name, lm.have_cn, best_cn);
                        std::string_view ttl = sv_at(title_dat, title_off, r);
                        update_min(lm.t_title, lm.have_t, ttl);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // Reduce
    LocalMin g;
    auto reduce_in = [](std::string& cur, bool& have, const std::string& v, bool hv) {
        if (!hv) return;
        if (!have || cur > v) { cur = v; have = true; }
    };
    for (auto& lm : locals) {
        reduce_in(g.cn_name,     g.have_cn, lm.cn_name,     lm.have_cn);
        reduce_in(g.mi_idx_info, g.have_mi, lm.mi_idx_info, lm.have_mi);
        reduce_in(g.t_title,     g.have_t,  lm.t_title,     lm.have_t);
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        fs::create_directories(results_dir);
        std::string out_path = results_dir + "/Q22d.csv";
        FILE* f = fopen(out_path.c_str(), "w");
        if (!f) { fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        fprintf(f, "movie_company,rating,western_violent_movie\n");
        auto write_field = [&](const std::string& v, bool have) {
            if (!have) { fprintf(f, ""); return; }
            // Minimal CSV escaping: quote if contains comma, quote, or newline
            bool need_q = false;
            for (char c : v) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_q = true; break; }
            }
            if (!need_q) {
                fwrite(v.data(), 1, v.size(), f);
            } else {
                fputc('"', f);
                for (char c : v) {
                    if (c == '"') fputc('"', f);
                    fputc(c, f);
                }
                fputc('"', f);
            }
        };
        write_field(g.cn_name, g.have_cn);
        fputc(',', f);
        write_field(g.mi_idx_info, g.have_mi);
        fputc(',', f);
        write_field(g.t_title, g.have_t);
        fputc('\n', f);
        fclose(f);
    }

    return 0;
}
