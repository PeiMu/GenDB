// Q7c — MIN(n.name), MIN(pi.info)
// Driver: person_info slice via CSR person_info__info_type_id for it.info='mini biography'
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <atomic>
#include <thread>
#include <algorithm>
#include <climits>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

namespace {

// Lex compare a varlen "candidate" (off/dat at row r) against a current best (off/len).
// Returns true if candidate is strictly less than current best.
static inline bool varlen_lt(const char* a_dat, size_t a_len,
                             const char* b_dat, size_t b_len) {
    size_t n = a_len < b_len ? a_len : b_len;
    int c = std::memcmp(a_dat, b_dat, n);
    if (c != 0) return c < 0;
    return a_len < b_len;
}

} // anon

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string outdir = argv[2];
    std::filesystem::create_directories(outdir);

    // ---- 1. Data loading: mmap all needed columns ----
    MmapColumn<int64_t> it_info_off;
    MmapColumn<char>    it_info_dat;
    MmapColumn<int64_t> lt_link_off;
    MmapColumn<char>    lt_link_dat;

    MmapColumn<int64_t> pi_info_off;
    MmapColumn<char>    pi_info_dat;
    MmapColumn<int64_t> pi_note_off;
    MmapColumn<int32_t> pi_person;
    MmapColumn<int32_t> pi_it_off;
    MmapColumn<int32_t> pi_it_rowids;

    MmapColumn<int64_t> name_off;
    MmapColumn<char>    name_dat;
    MmapColumn<int64_t> name_pcode_cf_off;
    MmapColumn<char>    name_pcode_cf_dat;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<int64_t> gender_dict_off;
    MmapColumn<char>    gender_dict_dat;

    MmapColumn<int32_t> an_off;
    MmapColumn<int64_t> an_name_off;
    MmapColumn<char>    an_name_dat;

    MmapColumn<int32_t> ci_p_off;
    MmapColumn<int32_t> ci_p_rowids;
    MmapColumn<int32_t> ci_movie;

    MmapColumn<int32_t> title_year;

    MmapColumn<int32_t> ml_lm_off;
    MmapColumn<int32_t> ml_lm_rowids;
    MmapColumn<int32_t> ml_link_type;

    {
        GENDB_PHASE("data_loading");

        it_info_off.open(gendb + "/info_type/info.off");
        it_info_dat.open(gendb + "/info_type/info.dat");
        lt_link_off.open(gendb + "/link_type/link.off");
        lt_link_dat.open(gendb + "/link_type/link.dat");

        pi_info_off.open(gendb + "/person_info/info.off");
        pi_info_dat.open(gendb + "/person_info/info.dat");
        pi_note_off.open(gendb + "/person_info/note.off");
        pi_person.open(gendb + "/person_info/person_id.bin");
        pi_it_off.open(gendb + "/_idx/person_info__info_type_id__offsets.bin");
        pi_it_rowids.open(gendb + "/_idx/person_info__info_type_id__rowids.bin");

        name_off.open(gendb + "/name/name.off");
        name_dat.open(gendb + "/name/name.dat");
        name_pcode_cf_off.open(gendb + "/name/name_pcode_cf.off");
        name_pcode_cf_dat.open(gendb + "/name/name_pcode_cf.dat");
        name_gender.open(gendb + "/name/gender.bin");
        gender_dict_off.open(gendb + "/name/gender.dict.off");
        gender_dict_dat.open(gendb + "/name/gender.dict.dat");

        an_off.open(gendb + "/_idx/aka_name__person_id__offsets.bin");
        an_name_off.open(gendb + "/aka_name/name.off");
        an_name_dat.open(gendb + "/aka_name/name.dat");

        ci_p_off.open(gendb + "/_idx/cast_info__person_id__offsets.bin");
        ci_p_rowids.open(gendb + "/_idx/cast_info__person_id__rowids.bin");
        ci_movie.open(gendb + "/cast_info/movie_id.bin");

        title_year.open(gendb + "/title/production_year.bin");

        ml_lm_off.open(gendb + "/_idx/movie_link__linked_movie_id__offsets.bin");
        ml_lm_rowids.open(gendb + "/_idx/movie_link__linked_movie_id__rowids.bin");
        ml_link_type.open(gendb + "/movie_link/link_type_id.bin");
    }

    // ---- 2. Resolve dim codes ----
    int32_t target_it_id = -1;
    {
        size_t n = it_info_off.count - 1; // number of rows
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = it_info_off[i], hi = it_info_off[i+1];
            std::string_view s(it_info_dat.data + lo, hi - lo);
            if (s == "mini biography") {
                target_it_id = (int32_t)(i + 1); // info_type.id is dense 1-based
                break;
            }
        }
        if (target_it_id < 0) {
            std::fprintf(stderr, "info_type 'mini biography' not found\n");
            return 2;
        }
    }

    std::array<bool, 32> target_lt_ids{};
    {
        size_t n = lt_link_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = lt_link_off[i], hi = lt_link_off[i+1];
            std::string_view s(lt_link_dat.data + lo, hi - lo);
            if (s == "references" || s == "referenced in" ||
                s == "features"   || s == "featured in") {
                size_t id = i + 1;
                if (id < target_lt_ids.size()) target_lt_ids[id] = true;
            }
        }
    }

    int8_t code_m = 0, code_f = 0;
    {
        size_t n = gender_dict_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            int64_t lo = gender_dict_off[i], hi = gender_dict_off[i+1];
            std::string_view s(gender_dict_dat.data + lo, hi - lo);
            if (s == "m") code_m = (int8_t)(i + 1);
            if (s == "f") code_f = (int8_t)(i + 1);
        }
    }

    // ---- 3. Determine driver slice [lo,hi) of pi rowids matching target_it_id ----
    int32_t pi_lo = pi_it_off[target_it_id];
    int32_t pi_hi = pi_it_off[target_it_id + 1];

    // ---- 4. Parallel scan ----
    struct LocalBest {
        // best name_id (1-based name.id)
        int32_t best_name_id;
        const char* best_name_ptr;
        size_t best_name_len;
        // best pi_row
        int32_t best_pi_row;
        const char* best_pi_info_ptr;
        size_t best_pi_info_len;
    };

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (nthreads > 12) nthreads = 12;

    std::vector<LocalBest> locals(nthreads);
    for (auto& l : locals) {
        l.best_name_id = -1; l.best_name_ptr = nullptr; l.best_name_len = 0;
        l.best_pi_row = -1; l.best_pi_info_ptr = nullptr; l.best_pi_info_len = 0;
    }

    {
        GENDB_PHASE("main_scan");

        int32_t total = pi_hi - pi_lo;
        if (total < 0) total = 0;

        // morsel size
        const int32_t MORSEL = 4096;
        std::atomic<int32_t> next_morsel{0};

        auto worker = [&](unsigned tid) {
            LocalBest& lb = locals[tid];
            while (true) {
                int32_t m = next_morsel.fetch_add(1, std::memory_order_relaxed);
                int32_t start = pi_lo + m * MORSEL;
                if (start >= pi_hi) break;
                int32_t end = start + MORSEL;
                if (end > pi_hi) end = pi_hi;

                for (int32_t j = start; j < end; ++j) {
                    int32_t pi_row = pi_it_rowids[j];

                    // pi.note IS NOT NULL ⇒ off[r+1] > off[r]
                    int64_t no = pi_note_off[pi_row];
                    int64_t no_n = pi_note_off[pi_row + 1];
                    if (no_n <= no) continue;

                    int32_t pid = pi_person[pi_row];
                    if (pid <= 0) continue;
                    int32_t name_idx = pid - 1; // dense PK

                    // name_pcode_cf BETWEEN 'A' AND 'F'
                    int64_t pc_lo = name_pcode_cf_off[name_idx];
                    int64_t pc_hi = name_pcode_cf_off[name_idx + 1];
                    size_t pc_len = (size_t)(pc_hi - pc_lo);
                    if (pc_len == 0) continue;
                    const char* pc = name_pcode_cf_dat.data + pc_lo;
                    // s >= "A"
                    if (pc[0] < 'A') continue;
                    // s <= "F": means s[0] < 'F' OR (s == "F")
                    if (pc[0] > 'F') continue;
                    if (pc[0] == 'F' && pc_len > 1) continue;

                    // gender check
                    int8_t g = name_gender[name_idx];
                    // name varlen lookup once
                    int64_t nm_lo = name_off[name_idx];
                    int64_t nm_hi = name_off[name_idx + 1];
                    size_t nm_len = (size_t)(nm_hi - nm_lo);
                    const char* nm = name_dat.data + nm_lo;
                    bool name_starts_A = (nm_len > 0) && (nm[0] == 'A');

                    bool gender_ok = false;
                    if (g == code_m) gender_ok = true;
                    else if (g == code_f && name_starts_A) gender_ok = true;
                    if (!gender_ok) continue;

                    // aka_name semi-join: per pid scan aka slice
                    int32_t an_l = an_off[pid];
                    int32_t an_h = an_off[pid + 1];
                    bool aka_ok = false;
                    for (int32_t r = an_l; r < an_h; ++r) {
                        int64_t aol = an_name_off[r];
                        int64_t aoh = an_name_off[r + 1];
                        size_t alen = (size_t)(aoh - aol);
                        if (alen == 0) continue;
                        const char* aptr = an_name_dat.data + aol;
                        if (aptr[0] == 'A') { aka_ok = true; break; }
                        if (std::memchr(aptr, 'a', alen) != nullptr) { aka_ok = true; break; }
                    }
                    if (!aka_ok) continue;

                    // Now we need at least one match through ci → title → ml chain.
                    // Iterate ci rows for this pid; for each t_id, check year & ml.
                    int32_t ci_l = ci_p_off[pid];
                    int32_t ci_h = ci_p_off[pid + 1];
                    bool full_match = false;
                    for (int32_t k = ci_l; k < ci_h && !full_match; ++k) {
                        int32_t ci_row = ci_p_rowids[k];
                        int32_t t_id = ci_movie[ci_row];
                        if (t_id <= 0) continue;
                        int32_t y = title_year[t_id - 1];
                        if (y == INT32_MIN) continue;
                        if (y < 1980 || y > 2010) continue;

                        int32_t ml_l = ml_lm_off[t_id];
                        int32_t ml_h = ml_lm_off[t_id + 1];
                        for (int32_t q = ml_l; q < ml_h; ++q) {
                            int32_t ml_row = ml_lm_rowids[q];
                            int32_t lt_id = ml_link_type[ml_row];
                            if (lt_id >= 0 && (size_t)lt_id < target_lt_ids.size()
                                && target_lt_ids[(size_t)lt_id]) {
                                full_match = true;
                                break;
                            }
                        }
                    }
                    if (!full_match) continue;

                    // Update MIN(n.name): candidate is (nm, nm_len)
                    if (lb.best_name_ptr == nullptr ||
                        varlen_lt(nm, nm_len, lb.best_name_ptr, lb.best_name_len)) {
                        lb.best_name_ptr = nm;
                        lb.best_name_len = nm_len;
                        lb.best_name_id = pid;
                    }

                    // Update MIN(pi.info): candidate from pi_row varlen
                    int64_t info_lo = pi_info_off[pi_row];
                    int64_t info_hi = pi_info_off[pi_row + 1];
                    size_t info_len = (size_t)(info_hi - info_lo);
                    const char* info_ptr = pi_info_dat.data + info_lo;
                    if (lb.best_pi_info_ptr == nullptr ||
                        varlen_lt(info_ptr, info_len, lb.best_pi_info_ptr, lb.best_pi_info_len)) {
                        lb.best_pi_info_ptr = info_ptr;
                        lb.best_pi_info_len = info_len;
                        lb.best_pi_row = pi_row;
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            threads.emplace_back(worker, t);
        }
        for (auto& th : threads) th.join();
    }

    // ---- 5. Reduce ----
    const char* best_name_ptr = nullptr; size_t best_name_len = 0;
    const char* best_info_ptr = nullptr; size_t best_info_len = 0;
    for (auto& l : locals) {
        if (l.best_name_ptr != nullptr) {
            if (best_name_ptr == nullptr ||
                varlen_lt(l.best_name_ptr, l.best_name_len, best_name_ptr, best_name_len)) {
                best_name_ptr = l.best_name_ptr;
                best_name_len = l.best_name_len;
            }
        }
        if (l.best_pi_info_ptr != nullptr) {
            if (best_info_ptr == nullptr ||
                varlen_lt(l.best_pi_info_ptr, l.best_pi_info_len, best_info_ptr, best_info_len)) {
                best_info_ptr = l.best_pi_info_ptr;
                best_info_len = l.best_pi_info_len;
            }
        }
    }

    // ---- 6. Output CSV ----
    {
        GENDB_PHASE("output");
        std::string outpath = outdir + "/Q7c.csv";
        FILE* fp = std::fopen(outpath.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 3; }
        std::fputs("cast_member_name,cast_member_info\n", fp);

        auto write_csv_field = [&](const char* s, size_t n) {
            // Quote if contains comma, quote, or newline
            bool needs_quote = false;
            for (size_t i = 0; i < n; ++i) {
                char c = s[i];
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
            }
            if (!needs_quote) {
                std::fwrite(s, 1, n, fp);
            } else {
                std::fputc('"', fp);
                for (size_t i = 0; i < n; ++i) {
                    char c = s[i];
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
            }
        };

        if (best_name_ptr) write_csv_field(best_name_ptr, best_name_len);
        std::fputc(',', fp);
        if (best_info_ptr) write_csv_field(best_info_ptr, best_info_len);
        std::fputc('\n', fp);
        std::fclose(fp);
    }

    return 0;
}
