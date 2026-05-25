// Q22c — MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// Generated implementation: title-driver scan with primary-CSR probes
// over movie_keyword, movie_info, movie_info_idx, movie_companies.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "cli_params.h"

// ---------- mmap helpers ----------
static const void* mmap_file_raw(const std::string& path, size_t& sz) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st{};
    if (fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    sz = (size_t)st.st_size;
    if (sz == 0) { ::close(fd); return nullptr; }
    void* p = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    return p;
}

template<typename T>
static const T* mmap_as(const std::string& path, size_t& count) {
    size_t sz = 0;
    const void* p = mmap_file_raw(path, sz);
    count = sz / sizeof(T);
    return reinterpret_cast<const T*>(p);
}

// ---------- LIKE pattern (handles %lit1%lit2%...%) ----------
struct LikePattern {
    bool starts_anchored = false;
    bool ends_anchored = false;
    std::vector<std::string> literals;
};

static LikePattern parse_like(const std::string& pat) {
    LikePattern lp;
    if (pat.empty()) {
        lp.starts_anchored = true;
        lp.ends_anchored = true;
        return lp;
    }
    lp.starts_anchored = pat.front() != '%';
    lp.ends_anchored = pat.back() != '%';
    size_t i = 0;
    while (i < pat.size()) {
        if (pat[i] == '%') { i++; continue; }
        size_t j = i;
        std::string lit;
        while (j < pat.size() && pat[j] != '%') {
            lit.push_back(pat[j]);
            j++;
        }
        lp.literals.push_back(std::move(lit));
        i = j;
    }
    return lp;
}

static inline bool match_like(const char* s, size_t slen, const LikePattern& lp) {
    if (lp.literals.empty()) {
        // pure "%" — matches anything; "" anchored both sides matches only empty
        return !(lp.starts_anchored && lp.ends_anchored) || slen == 0;
    }
    size_t pos = 0;
    for (size_t i = 0; i < lp.literals.size(); i++) {
        const std::string& lit = lp.literals[i];
        const bool first = (i == 0);
        const bool last = (i + 1 == lp.literals.size());
        if (first && lp.starts_anchored) {
            if (slen - pos < lit.size()) return false;
            if (std::memcmp(s + pos, lit.data(), lit.size()) != 0) return false;
            pos += lit.size();
        } else if (last && lp.ends_anchored) {
            if (slen < lit.size()) return false;
            size_t end_pos = slen - lit.size();
            if (end_pos < pos) return false;
            if (std::memcmp(s + end_pos, lit.data(), lit.size()) != 0) return false;
            // also must be reachable from pos; but for last anchored we don't need it to match earlier
            pos = slen;
        } else {
            const void* found = memmem(s + pos, slen - pos, lit.data(), lit.size());
            if (!found) return false;
            pos = (const char*)found - s + lit.size();
        }
    }
    return true;
}

// ---------- min helpers ----------
static inline bool lex_less(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    int c = (n > 0) ? std::memcmp(a.data(), b.data(), n) : 0;
    if (c != 0) return c < 0;
    return a.size() < b.size();
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [params]\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];

    // Parse parameters
    int64_t production_year_lower = gendb::parse_int_arg(argc, argv, "--production_year_lower", 2005);
    double info_upper_d = gendb::parse_double_arg(argc, argv, "--info_upper", 8.5);
    std::string note_pattern = gendb::parse_string_arg(argc, argv, "--note_pattern", "%(200%)%");
    std::string note_pattern_2 = gendb::parse_string_arg(argc, argv, "--note_pattern_2", "%(USA)%");
    std::string info_eq = gendb::parse_string_arg(argc, argv, "--info_eq", "rating");
    std::string country_code_neq = gendb::parse_string_arg(argc, argv, "--country_code_neq", "[us]");
    std::string info_eq_2 = gendb::parse_string_arg(argc, argv, "--info_eq_2", "countries");

    // Convert info_upper double to lexicographic string for mi_idx.info < threshold
    // The data stores rating as "X.Y" strings; lex compare equals numeric compare in [0.0, 9.9].
    char info_upper_buf[32];
    std::snprintf(info_upper_buf, sizeof(info_upper_buf), "%g", info_upper_d);
    std::string info_upper_str = info_upper_buf;

    LikePattern lp_match = parse_like(note_pattern);     // must match
    LikePattern lp_exclude = parse_like(note_pattern_2); // must NOT match

    // ---------- Phase 1: data loading (mmap) ----------
    const int32_t* title_id = nullptr;
    const int32_t* title_kind_id = nullptr;
    const int32_t* title_prod_year = nullptr;
    const uint64_t* title_title_off = nullptr;
    const char* title_title_dat = nullptr;
    size_t N_title = 0;

    const int32_t* mk_movie_id = nullptr;
    const int32_t* mk_keyword_id = nullptr;
    const int32_t* mk_off = nullptr;  // primary CSR (int32 cumulative)
    size_t N_mk = 0;
    (void)mk_movie_id;

    const int32_t* mi_movie_id = nullptr;
    const int32_t* mi_info_type_id = nullptr;
    const uint64_t* mi_info_off = nullptr;
    const char* mi_info_dat = nullptr;
    const int32_t* mi_off = nullptr;
    size_t N_mi = 0;
    (void)mi_movie_id;

    const int32_t* midx_movie_id = nullptr;
    const int32_t* midx_info_type_id = nullptr;
    const uint64_t* midx_info_off = nullptr;
    const char* midx_info_dat = nullptr;
    const int32_t* midx_off = nullptr;
    size_t N_midx = 0;
    (void)midx_movie_id;

    const int32_t* mc_movie_id = nullptr;
    const int32_t* mc_company_id = nullptr;
    const uint64_t* mc_note_off = nullptr;
    const char* mc_note_dat = nullptr;
    const int32_t* mc_off = nullptr;
    size_t N_mc = 0;
    (void)mc_movie_id;

    const int16_t* cn_country_code = nullptr;  // int16 dict codes
    const uint64_t* cn_name_off = nullptr;
    const char* cn_name_dat = nullptr;
    size_t N_cn = 0;
    const uint64_t* cn_cc_dict_off = nullptr;
    const char* cn_cc_dict_dat = nullptr;
    size_t N_cn_cc_dict = 0;

    // Dim text columns
    const int32_t* info_type_id_arr = nullptr;
    const uint64_t* info_type_info_off = nullptr;
    const char* info_type_info_dat = nullptr;
    size_t N_info_type = 0;

    const int32_t* kind_type_id_arr = nullptr;
    const uint64_t* kind_type_kind_off = nullptr;
    const char* kind_type_kind_dat = nullptr;
    size_t N_kind_type = 0;

    const int32_t* keyword_id_arr = nullptr;
    const uint64_t* keyword_keyword_off = nullptr;
    const char* keyword_keyword_dat = nullptr;
    size_t N_keyword = 0;

    {
        GENDB_PHASE("data_loading");
        size_t c = 0;
        // title
        title_id = mmap_as<int32_t>(gendb + "/title/id.bin", c); N_title = c;
        title_kind_id = mmap_as<int32_t>(gendb + "/title/kind_id.bin", c);
        title_prod_year = mmap_as<int32_t>(gendb + "/title/production_year.bin", c);
        title_title_off = mmap_as<uint64_t>(gendb + "/title/title.off", c);
        title_title_dat = mmap_as<char>(gendb + "/title/title.dat", c);

        // movie_keyword
        mk_movie_id = mmap_as<int32_t>(gendb + "/movie_keyword/movie_id.bin", c); N_mk = c;
        mk_keyword_id = mmap_as<int32_t>(gendb + "/movie_keyword/keyword_id.bin", c);
        mk_off = mmap_as<int32_t>(gendb + "/_idx/movie_keyword__movie_id__offsets.bin", c);

        // movie_info
        mi_movie_id = mmap_as<int32_t>(gendb + "/movie_info/movie_id.bin", c); N_mi = c;
        mi_info_type_id = mmap_as<int32_t>(gendb + "/movie_info/info_type_id.bin", c);
        mi_info_off = mmap_as<uint64_t>(gendb + "/movie_info/info.off", c);
        mi_info_dat = mmap_as<char>(gendb + "/movie_info/info.dat", c);
        mi_off = mmap_as<int32_t>(gendb + "/_idx/movie_info__movie_id__offsets.bin", c);

        // movie_info_idx
        midx_movie_id = mmap_as<int32_t>(gendb + "/movie_info_idx/movie_id.bin", c); N_midx = c;
        midx_info_type_id = mmap_as<int32_t>(gendb + "/movie_info_idx/info_type_id.bin", c);
        midx_info_off = mmap_as<uint64_t>(gendb + "/movie_info_idx/info.off", c);
        midx_info_dat = mmap_as<char>(gendb + "/movie_info_idx/info.dat", c);
        midx_off = mmap_as<int32_t>(gendb + "/_idx/movie_info_idx__movie_id__offsets.bin", c);

        // movie_companies
        mc_movie_id = mmap_as<int32_t>(gendb + "/movie_companies/movie_id.bin", c); N_mc = c;
        mc_company_id = mmap_as<int32_t>(gendb + "/movie_companies/company_id.bin", c);
        mc_note_off = mmap_as<uint64_t>(gendb + "/movie_companies/note.off", c);
        mc_note_dat = mmap_as<char>(gendb + "/movie_companies/note.dat", c);
        mc_off = mmap_as<int32_t>(gendb + "/_idx/movie_companies__movie_id__offsets.bin", c);

        // company_name
        cn_country_code = mmap_as<int16_t>(gendb + "/company_name/country_code.bin", c); N_cn = c;
        cn_name_off = mmap_as<uint64_t>(gendb + "/company_name/name.off", c);
        cn_name_dat = mmap_as<char>(gendb + "/company_name/name.dat", c);
        cn_cc_dict_off = mmap_as<uint64_t>(gendb + "/company_name/country_code.dict.off", c);
        cn_cc_dict_dat = mmap_as<char>(gendb + "/company_name/country_code.dict.dat", c);
        N_cn_cc_dict = c - 1;

        // info_type
        info_type_id_arr = mmap_as<int32_t>(gendb + "/info_type/id.bin", c); N_info_type = c;
        info_type_info_off = mmap_as<uint64_t>(gendb + "/info_type/info.off", c);
        info_type_info_dat = mmap_as<char>(gendb + "/info_type/info.dat", c);

        // kind_type
        kind_type_id_arr = mmap_as<int32_t>(gendb + "/kind_type/id.bin", c); N_kind_type = c;
        kind_type_kind_off = mmap_as<uint64_t>(gendb + "/kind_type/kind.off", c);
        kind_type_kind_dat = mmap_as<char>(gendb + "/kind_type/kind.dat", c);

        // keyword
        keyword_id_arr = mmap_as<int32_t>(gendb + "/keyword/id.bin", c); N_keyword = c;
        keyword_keyword_off = mmap_as<uint64_t>(gendb + "/keyword/keyword.off", c);
        keyword_keyword_dat = mmap_as<char>(gendb + "/keyword/keyword.dat", c);
    }

    // ---------- Phase 2: dim literal resolution ----------
    int32_t it1_id = -1, it2_id = -1;
    for (size_t r = 0; r < N_info_type; r++) {
        std::string_view s(info_type_info_dat + info_type_info_off[r],
                           info_type_info_off[r+1] - info_type_info_off[r]);
        if (s == info_eq_2) it1_id = info_type_id_arr[r];
        if (s == info_eq) it2_id = info_type_id_arr[r];
    }
    if (it1_id < 0 || it2_id < 0) {
        std::fprintf(stderr, "Could not resolve info_type literals\n");
        return 1;
    }

    // kind_type IN ('movie','episode')
    std::vector<int32_t> kt_ids;
    const std::unordered_set<std::string> kt_set = {"movie", "episode"};
    for (size_t r = 0; r < N_kind_type; r++) {
        std::string_view s(kind_type_kind_dat + kind_type_kind_off[r],
                           kind_type_kind_off[r+1] - kind_type_kind_off[r]);
        if (kt_set.count(std::string(s))) kt_ids.push_back(kind_type_id_arr[r]);
    }

    // keyword IN ('murder','murder-in-title','blood','violence')
    std::vector<int32_t> k_ids;
    const std::unordered_set<std::string> k_set = {"murder", "murder-in-title", "blood", "violence"};
    for (size_t r = 0; r < N_keyword; r++) {
        std::string_view s(keyword_keyword_dat + keyword_keyword_off[r],
                           keyword_keyword_off[r+1] - keyword_keyword_off[r]);
        if (k_set.count(std::string(s))) k_ids.push_back(keyword_id_arr[r]);
    }
    // Build bitset over keyword_id space for fast membership test
    int32_t max_kid = 0;
    for (int32_t v : k_ids) if (v > max_kid) max_kid = v;
    std::vector<uint8_t> k_id_bitset((size_t)max_kid + 1, 0);
    for (int32_t v : k_ids) k_id_bitset[v] = 1;

    // country_code dict: find code for '[us]'
    int16_t us_code = 0;
    for (size_t r = 0; r < N_cn_cc_dict; r++) {
        std::string_view s(cn_cc_dict_dat + cn_cc_dict_off[r],
                           cn_cc_dict_off[r+1] - cn_cc_dict_off[r]);
        // dict code i references entry i-1 (code 0 = NULL)
        if (s == country_code_neq) { us_code = (int16_t)(r + 1); break; }
    }
    // if us_code stays 0, no row would equal '[us]' anyway; bitset all valid

    // country_set for mi.info
    const std::unordered_set<std::string> country_set = {
        "Sweden","Norway","Germany","Denmark","Swedish","Danish",
        "Norwegian","German","USA","American"
    };

    // ---------- Phase 3: main scan ----------
    std::string g_min_cn_name;
    std::string g_min_midx_info;
    std::string g_min_t_title;
    bool g_have_cn = false, g_have_midx = false, g_have_t = false;
    std::mutex g_mu;

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 12) nthreads = 12;

    const size_t MORSEL = 16384;
    std::atomic<size_t> next_morsel{0};

    auto worker = [&](int /*tid*/) {
        std::string l_min_cn_name;
        std::string l_min_midx_info;
        std::string l_min_t_title;
        bool have_cn = false, have_midx = false, have_t = false;

        while (true) {
            size_t start = next_morsel.fetch_add(MORSEL, std::memory_order_relaxed);
            if (start >= N_title) break;
            size_t end = std::min(start + MORSEL, N_title);

            for (size_t r = start; r < end; r++) {
                // Filter: production_year > production_year_lower (skip NULL = INT32_MIN)
                int32_t py = title_prod_year[r];
                if (py == INT32_MIN || py <= production_year_lower) continue;

                // Filter: kind_id IN kt_ids
                int32_t kid = title_kind_id[r];
                bool kind_ok = false;
                for (int32_t v : kt_ids) if (kid == v) { kind_ok = true; break; }
                if (!kind_ok) continue;

                int32_t tid = title_id[r];

                // semi-join mk: keyword_id IN k_ids
                {
                    int32_t lo = mk_off[tid], hi = mk_off[tid+1];
                    bool found = false;
                    for (int32_t i = lo; i < hi; i++) {
                        int32_t k = mk_keyword_id[i];
                        if (k >= 0 && k <= max_kid && k_id_bitset[k]) { found = true; break; }
                    }
                    if (!found) continue;
                }

                // semi-join mi: info_type_id == it1_id AND info IN country_set
                {
                    int32_t lo = mi_off[tid], hi = mi_off[tid+1];
                    bool found = false;
                    for (int32_t i = lo; i < hi; i++) {
                        if (mi_info_type_id[i] != it1_id) continue;
                        uint64_t s = mi_info_off[i];
                        uint64_t e = mi_info_off[i+1];
                        std::string sv(mi_info_dat + s, mi_info_dat + e);
                        if (country_set.count(sv)) { found = true; break; }
                    }
                    if (!found) continue;
                }

                // mi_idx: collect surviving rows for MIN
                std::vector<int32_t> midx_surv;
                {
                    int32_t lo = midx_off[tid], hi = midx_off[tid+1];
                    for (int32_t i = lo; i < hi; i++) {
                        if (midx_info_type_id[i] != it2_id) continue;
                        uint64_t s = midx_info_off[i];
                        uint64_t e = midx_info_off[i+1];
                        std::string_view sv(midx_info_dat + s, e - s);
                        if (lex_less(sv, info_upper_str)) {
                            midx_surv.push_back(i);
                        }
                    }
                }
                if (midx_surv.empty()) continue;

                // mc: collect surviving rows with cn lookups
                std::vector<int32_t> cn_rows;
                {
                    int32_t lo = mc_off[tid], hi = mc_off[tid+1];
                    for (int32_t i = lo; i < hi; i++) {
                        uint64_t s = mc_note_off[i];
                        uint64_t e = mc_note_off[i+1];
                        size_t nlen = (size_t)(e - s);
                        if (nlen == 0) continue;  // NULL note doesn't match LIKE
                        const char* np = mc_note_dat + s;
                        // must match note_pattern AND must NOT match note_pattern_2
                        if (!match_like(np, nlen, lp_match)) continue;
                        if (match_like(np, nlen, lp_exclude)) continue;

                        // cn lookup: company_name.id is dense 1..N
                        int32_t cid = mc_company_id[i];
                        if (cid <= 0 || (size_t)cid > N_cn) continue;
                        int32_t cn_r = cid - 1;
                        int16_t cc = cn_country_code[cn_r];
                        // country_code != '[us]': must be non-null and != us_code
                        if (cc == 0) continue;
                        if (us_code != 0 && cc == us_code) continue;
                        cn_rows.push_back(cn_r);
                    }
                }
                if (cn_rows.empty()) continue;

                // Update mins
                {
                    uint64_t s = title_title_off[r];
                    uint64_t e = title_title_off[r+1];
                    std::string_view tv(title_title_dat + s, e - s);
                    if (!have_t || lex_less(tv, l_min_t_title)) {
                        l_min_t_title.assign(tv);
                        have_t = true;
                    }
                }
                for (int32_t cn_r : cn_rows) {
                    uint64_t s = cn_name_off[cn_r];
                    uint64_t e = cn_name_off[cn_r+1];
                    std::string_view nv(cn_name_dat + s, e - s);
                    if (nv.empty()) continue;  // NULL
                    if (!have_cn || lex_less(nv, l_min_cn_name)) {
                        l_min_cn_name.assign(nv);
                        have_cn = true;
                    }
                }
                for (int32_t mi_r : midx_surv) {
                    uint64_t s = midx_info_off[mi_r];
                    uint64_t e = midx_info_off[mi_r+1];
                    std::string_view iv(midx_info_dat + s, e - s);
                    if (iv.empty()) continue;
                    if (!have_midx || lex_less(iv, l_min_midx_info)) {
                        l_min_midx_info.assign(iv);
                        have_midx = true;
                    }
                }
            }
        }

        // merge into global
        std::lock_guard<std::mutex> lk(g_mu);
        if (have_t && (!g_have_t || lex_less(l_min_t_title, g_min_t_title))) {
            g_min_t_title = l_min_t_title; g_have_t = true;
        }
        if (have_cn && (!g_have_cn || lex_less(l_min_cn_name, g_min_cn_name))) {
            g_min_cn_name = l_min_cn_name; g_have_cn = true;
        }
        if (have_midx && (!g_have_midx || lex_less(l_min_midx_info, g_min_midx_info))) {
            g_min_midx_info = l_min_midx_info; g_have_midx = true;
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> ts;
        ts.reserve(nthreads);
        for (unsigned i = 0; i < nthreads; i++) ts.emplace_back(worker, (int)i);
        for (auto& t : ts) t.join();
    }

    // ---------- Phase 4: output ----------
    {
        GENDB_PHASE("output");
        // Ensure results dir exists
        std::string mkcmd = "mkdir -p '" + results + "'";
        int rc = std::system(mkcmd.c_str());
        (void)rc;
        std::string out_path = results + "/Q22c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot open output %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "movie_company,rating,western_violent_movie\n");
        auto write_field = [&](const std::string& s, bool last) {
            // CSV: if has comma or quote, quote it.
            bool needs_quote = false;
            for (char c : s) if (c == ',' || c == '"' || c == '\n') { needs_quote = true; break; }
            if (needs_quote) {
                std::fputc('"', f);
                for (char c : s) {
                    if (c == '"') std::fputs("\"\"", f);
                    else std::fputc(c, f);
                }
                std::fputc('"', f);
            } else {
                std::fwrite(s.data(), 1, s.size(), f);
            }
            std::fputc(last ? '\n' : ',', f);
        };
        write_field(g_have_cn ? g_min_cn_name : std::string(""), false);
        write_field(g_have_midx ? g_min_midx_info : std::string(""), false);
        write_field(g_have_t ? g_min_t_title : std::string(""), true);
        std::fclose(f);
    }

    return 0;
}
