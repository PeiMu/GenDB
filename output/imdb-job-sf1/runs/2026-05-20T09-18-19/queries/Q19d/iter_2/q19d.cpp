// Q19d - voice actress / release dates / US companies / production_year > 2000
// Strategy: drive from cast_info (largest, most-selective conjunctive predicate on ci.note IN 4-voice strings)
// Apply cheap predicates first, then probe indexed dimensions.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// ---- file helpers ----
static size_t file_size(const std::string& p) {
    struct stat st; if (stat(p.c_str(), &st) < 0) { std::perror(p.c_str()); std::exit(1); } return st.st_size;
}

static void* map_ro(const std::string& p, size_t& sz) {
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(p.c_str()); std::exit(1); }
    struct stat st; fstat(fd, &st); sz = st.st_size;
    if (sz == 0) { close(fd); return nullptr; }
    void* m = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
    close(fd);
    return m;
}

// Resolve a varlen value to its row id (1-based dense PK). Returns -1 if not found.
static int32_t resolve_varlen_id(const std::string& dir, const std::string& col, const std::string& target) {
    size_t sz_off, sz_dat;
    auto* off = (const int64_t*) map_ro(dir + "/" + col + ".off", sz_off);
    auto* dat = (const char*)   map_ro(dir + "/" + col + ".dat", sz_dat);
    size_t n = sz_off / sizeof(int64_t) - 1;
    int32_t found = -1;
    for (size_t i = 0; i < n; i++) {
        size_t s = off[i], e = off[i+1];
        size_t len = e - s;
        if (len == target.size() && std::memcmp(dat + s, target.data(), len) == 0) {
            found = (int32_t)(i + 1); // dense PK id = row+1
            break;
        }
    }
    return found;
}

// Resolve a dict code: find dict entry index (1-based) where dict[i] == target.
// Returns -1 if missing.
static int32_t resolve_dict_code(const std::string& dir, const std::string& col, const std::string& target) {
    size_t sz_off, sz_dat;
    auto* off = (const int64_t*) map_ro(dir + "/" + col + ".dict.off", sz_off);
    auto* dat = (const char*)    map_ro(dir + "/" + col + ".dict.dat", sz_dat);
    size_t k = sz_off / sizeof(int64_t) - 1;
    for (size_t i = 0; i < k; i++) {
        size_t s = off[i], e = off[i+1];
        size_t len = e - s;
        if (len == target.size() && std::memcmp(dat + s, target.data(), len) == 0) {
            return (int32_t)(i + 1); // dict code = index+1
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const std::string gendb_dir   = argv[1];
    const std::string results_dir = argv[2];

    // --- Resolve dim ids ---
    int32_t rt_actress = -1;
    int32_t it_rd      = -1;
    int32_t cc_us_code = -1;     // dict code (int16)
    int32_t g_f_code   = -1;     // dict code (int8)
    {
        GENDB_PHASE("resolve_dims");
        rt_actress = resolve_varlen_id(gendb_dir + "/role_type", "role", "actress");
        it_rd      = resolve_varlen_id(gendb_dir + "/info_type", "info", "release dates");
        cc_us_code = resolve_dict_code(gendb_dir + "/company_name", "country_code", "[us]");
        g_f_code   = resolve_dict_code(gendb_dir + "/name", "gender", "f");
        if (rt_actress < 0 || it_rd < 0 || cc_us_code < 0 || g_f_code < 0) {
            std::fprintf(stderr, "Dim resolution failed: rt=%d it=%d cc=%d g=%d\n",
                         rt_actress, it_rd, cc_us_code, g_f_code);
            return 1;
        }
    }

    // --- Mmap data ---
    MmapColumn<int16_t> cn_country_code(gendb_dir + "/company_name/country_code.bin");
    MmapColumn<int8_t>  n_gender(gendb_dir + "/name/gender.bin");
    MmapColumn<int32_t> t_year(gendb_dir + "/title/production_year.bin");

    // title varlen
    MmapColumn<int64_t> t_title_off(gendb_dir + "/title/title.off");
    MmapColumn<char>    t_title_dat(gendb_dir + "/title/title.dat");

    // name varlen
    MmapColumn<int64_t> n_name_off(gendb_dir + "/name/name.off");
    MmapColumn<char>    n_name_dat(gendb_dir + "/name/name.dat");

    // movie_info index + info_type_id
    MmapColumn<int32_t> mi_off(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
    MmapColumn<int32_t> mi_info_type_id(gendb_dir + "/movie_info/info_type_id.bin");

    // movie_companies index + company_id
    MmapColumn<int32_t> mc_off(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
    MmapColumn<int32_t> mc_company_id(gendb_dir + "/movie_companies/company_id.bin");

    // aka_name index (existence)
    MmapColumn<int32_t> an_off(gendb_dir + "/_idx/aka_name__person_id__offsets.bin");

    // cast_info columns
    MmapColumn<int32_t> ci_movie_id(gendb_dir + "/cast_info/movie_id.bin");
    MmapColumn<int32_t> ci_person_id(gendb_dir + "/cast_info/person_id.bin");
    MmapColumn<int32_t> ci_role_id(gendb_dir + "/cast_info/role_id.bin");
    MmapColumn<int32_t> ci_person_role_id(gendb_dir + "/cast_info/person_role_id.bin");
    MmapColumn<int64_t> ci_note_off(gendb_dir + "/cast_info/note.off");
    MmapColumn<char>    ci_note_dat(gendb_dir + "/cast_info/note.dat");

    const size_t ci_rows = ci_movie_id.count;

    // --- Build cn_us_bitset (company id is 1-based dense; size = num companies) ---
    const size_t n_companies = cn_country_code.count;
    std::vector<uint64_t> cn_us_bits((n_companies + 63) / 64, 0);
    {
        GENDB_PHASE("build_cn_bitset");
        int16_t target = (int16_t) cc_us_code;
        for (size_t i = 0; i < n_companies; i++) {
            if (cn_country_code.data[i] == target) {
                cn_us_bits[i >> 6] |= (1ULL << (i & 63));
            }
        }
    }
    auto cn_us_test = [&](int32_t company_id) -> bool {
        // company_id is 1-based dense PK, index = company_id - 1
        uint32_t idx = (uint32_t)(company_id - 1);
        if (idx >= n_companies) return false;
        return (cn_us_bits[idx >> 6] >> (idx & 63)) & 1ULL;
    };

    // --- Voice notes: 4 string literals ---
    const char* V1 = "(voice)";                       size_t L1 = std::strlen(V1);
    const char* V2 = "(voice: Japanese version)";    size_t L2 = std::strlen(V2);
    const char* V3 = "(voice) (uncredited)";         size_t L3 = std::strlen(V3);
    const char* V4 = "(voice: English version)";    size_t L4 = std::strlen(V4);

    // --- Per-thread min state ---
    struct ThreadState {
        int32_t best_pid = 0;
        const char* best_name = nullptr;
        size_t best_name_len = 0;
        int32_t best_mv = 0;
        const char* best_title = nullptr;
        size_t best_title_len = 0;
    };

    auto cmp_lt = [](const char* a, size_t la, const char* b, size_t lb) -> bool {
        size_t l = la < lb ? la : lb;
        int c = std::memcmp(a, b, l);
        if (c != 0) return c < 0;
        return la < lb;
    };

    // --- Parallel scan ---
    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > 12) nthreads = 12;

    std::vector<ThreadState> states(nthreads);

    {
        GENDB_PHASE("main_scan");

        // Morsel-driven via atomic counter
        const size_t MORSEL = 1 << 18; // 256K rows
        std::atomic<size_t> next_morsel{0};
        const size_t n_morsels = (ci_rows + MORSEL - 1) / MORSEL;

        auto worker = [&](unsigned tid) {
            ThreadState& st = states[tid];

            while (true) {
                size_t m = next_morsel.fetch_add(1, std::memory_order_relaxed);
                if (m >= n_morsels) break;
                size_t lo = m * MORSEL;
                size_t hi = std::min(lo + MORSEL, ci_rows);

                for (size_t r = lo; r < hi; r++) {
                    // (1) role_id == rt_actress
                    if (ci_role_id.data[r] != rt_actress) continue;
                    // (2) person_role_id != INT32_MIN
                    int32_t prid = ci_person_role_id.data[r];
                    if (prid == INT32_MIN) continue;
                    // (3) note IN voice-set
                    int64_t ns = ci_note_off.data[r];
                    int64_t ne = ci_note_off.data[r + 1];
                    size_t nlen = (size_t)(ne - ns);
                    const char* nptr = ci_note_dat.data + ns;
                    bool note_ok = false;
                    if (nlen == L1) {
                        if (std::memcmp(nptr, V1, L1) == 0) note_ok = true;
                    } else if (nlen == L2) {
                        if (std::memcmp(nptr, V2, L2) == 0) note_ok = true;
                    } else if (nlen == L3) {
                        if (std::memcmp(nptr, V3, L3) == 0) note_ok = true;
                    } else if (nlen == L4) {
                        if (std::memcmp(nptr, V4, L4) == 0) note_ok = true;
                    }
                    if (!note_ok) continue;

                    int32_t pid = ci_person_id.data[r];
                    // (4) gender == 'f'
                    if ((uint32_t)(pid - 1) >= n_gender.count) continue;
                    if (n_gender.data[pid - 1] != (int8_t) g_f_code) continue;

                    int32_t mv = ci_movie_id.data[r];
                    // (5) title.production_year > 2000 AND != INT32_MIN
                    if ((uint32_t)(mv - 1) >= t_year.count) continue;
                    int32_t py = t_year.data[mv - 1];
                    if (py == INT32_MIN || py <= 2000) continue;

                    // (6) mi existence with info_type_id == it_rd
                    {
                        int32_t s = mi_off.data[mv];
                        int32_t e = mi_off.data[mv + 1];
                        bool found = false;
                        for (int32_t i = s; i < e; i++) {
                            if (mi_info_type_id.data[i] == it_rd) { found = true; break; }
                        }
                        if (!found) continue;
                    }

                    // (7) mc existence with cn_us
                    {
                        int32_t s = mc_off.data[mv];
                        int32_t e = mc_off.data[mv + 1];
                        bool found = false;
                        for (int32_t i = s; i < e; i++) {
                            if (cn_us_test(mc_company_id.data[i])) { found = true; break; }
                        }
                        if (!found) continue;
                    }

                    // (8) aka_name existence: off[pid+1] > off[pid]
                    {
                        int32_t s = an_off.data[pid];
                        int32_t e = an_off.data[pid + 1];
                        if (e <= s) continue;
                    }

                    // --- Match! Update mins ---
                    // name lookup
                    int64_t s_n = n_name_off.data[pid - 1];
                    int64_t e_n = n_name_off.data[pid];
                    const char* name_ptr = n_name_dat.data + s_n;
                    size_t name_len = (size_t)(e_n - s_n);
                    if (st.best_pid == 0 || cmp_lt(name_ptr, name_len, st.best_name, st.best_name_len)) {
                        st.best_pid = pid;
                        st.best_name = name_ptr;
                        st.best_name_len = name_len;
                    }

                    // title lookup
                    int64_t s_t = t_title_off.data[mv - 1];
                    int64_t e_t = t_title_off.data[mv];
                    const char* title_ptr = t_title_dat.data + s_t;
                    size_t title_len = (size_t)(e_t - s_t);
                    if (st.best_mv == 0 || cmp_lt(title_ptr, title_len, st.best_title, st.best_title_len)) {
                        st.best_mv = mv;
                        st.best_title = title_ptr;
                        st.best_title_len = title_len;
                    }
                }
            }
        };

        std::vector<std::thread> ts;
        ts.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; t++) ts.emplace_back(worker, t);
        for (auto& th : ts) th.join();
    }

    // --- Reduce ---
    const char* min_name = nullptr; size_t min_name_len = 0;
    const char* min_title = nullptr; size_t min_title_len = 0;
    for (auto& s : states) {
        if (s.best_pid != 0) {
            if (!min_name || cmp_lt(s.best_name, s.best_name_len, min_name, min_name_len)) {
                min_name = s.best_name; min_name_len = s.best_name_len;
            }
        }
        if (s.best_mv != 0) {
            if (!min_title || cmp_lt(s.best_title, s.best_title_len, min_title, min_title_len)) {
                min_title = s.best_title; min_title_len = s.best_title_len;
            }
        }
    }

    // --- Output CSV ---
    {
        GENDB_PHASE("output");
        // Ensure results dir exists (caller usually creates)
        mkdir(results_dir.c_str(), 0755); // ignore if exists

        std::string out_path = results_dir + "/Q19d.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::perror(out_path.c_str()); return 1; }
        std::fprintf(f, "voicing_actress,jap_engl_voiced_movie\n");

        auto write_csv_field = [&](const char* s, size_t l) {
            // Quote if contains comma, quote, or newline; double internal quotes.
            bool need_quote = false;
            for (size_t i = 0; i < l; i++) {
                char c = s[i];
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
            }
            if (need_quote) {
                std::fputc('"', f);
                for (size_t i = 0; i < l; i++) {
                    char c = s[i];
                    if (c == '"') { std::fputc('"', f); std::fputc('"', f); }
                    else std::fputc(c, f);
                }
                std::fputc('"', f);
            } else {
                std::fwrite(s, 1, l, f);
            }
        };

        if (min_name) write_csv_field(min_name, min_name_len);
        std::fputc(',', f);
        if (min_title) write_csv_field(min_title, min_title_len);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
