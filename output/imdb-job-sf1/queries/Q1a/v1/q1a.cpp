// Q1a: SELECT MIN(mc.note), MIN(t.title), MIN(t.production_year)
// Star-join on title (driver). Probe mi_idx (top-250) then mc (production companies + LIKE).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static std::string path_join(const std::string& a, const std::string& b) {
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

// Scan a dense-PK varlen text column and return the id (row index + 1)
// of the row whose text exactly matches `target`. Returns -1 if not found.
static int32_t resolve_dense_pk_varlen(const int64_t* off, const char* dat, size_t nrows,
                                       const char* target, size_t tgt_len) {
    for (size_t i = 0; i < nrows; ++i) {
        int64_t lo = off[i];
        int64_t hi = off[i + 1];
        size_t len = (size_t)(hi - lo);
        if (len == tgt_len && std::memcmp(dat + lo, target, tgt_len) == 0) {
            return (int32_t)(i + 1);
        }
    }
    return -1;
}

static inline bool slice_contains(const char* hay, size_t hay_len, const char* needle, size_t n_len) {
    if (n_len == 0) return true;
    if (hay_len < n_len) return false;
    return memmem(hay, hay_len, needle, n_len) != nullptr;
}

// Per-thread state for the 3-min aggregation.
struct ThreadAgg {
    bool has_note = false;
    std::string_view min_note;   // view into mmap'd note.dat
    bool has_title = false;
    std::string_view min_title;  // view into mmap'd title.dat
    bool has_year = false;
    int32_t min_year = INT32_MAX;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    init_date_tables();
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];

    // No named parameters in Q1a, but accept overrides if supplied.
    std::string p_ct_kind = parse_string_arg(argc, argv, "--ct_kind", std::string("production companies"));
    std::string p_it_info = parse_string_arg(argc, argv, "--it_info", std::string("top 250 rank"));
    std::string p_neg_pat  = parse_string_arg(argc, argv, "--neg_pat",  std::string("(as Metro-Goldwyn-Mayer Pictures)"));
    std::string p_pos_pat1 = parse_string_arg(argc, argv, "--pos_pat1", std::string("(co-production)"));
    std::string p_pos_pat2 = parse_string_arg(argc, argv, "--pos_pat2", std::string("(presents)"));

    // Ensure results directory exists
    {
        std::string mkcmd = "mkdir -p '" + results_dir + "'";
        if (system(mkcmd.c_str()) != 0) {
            // Don't fail catastrophically; the directory may already exist
        }
    }

    GENDB_PHASE("total");

    // ---- Load (mmap) all required columns/indexes ----
    MmapColumn<int64_t> ct_kind_off, it_info_off, title_off, mc_note_off;
    MmapColumn<char>    ct_kind_dat, it_info_dat, title_dat, mc_note_dat;
    MmapColumn<int32_t> mc_company_type_id, mc_movie_id_off_idx;
    MmapColumn<int32_t> mi_info_type_id,   mi_movie_id_off_idx;
    MmapColumn<int32_t> title_prod_year;

    {
        GENDB_PHASE("data_loading");
        ct_kind_off.open(path_join(gendb_dir, "company_type/kind.off"));
        ct_kind_dat.open(path_join(gendb_dir, "company_type/kind.dat"));
        it_info_off.open(path_join(gendb_dir, "info_type/info.off"));
        it_info_dat.open(path_join(gendb_dir, "info_type/info.dat"));
        title_off.open(path_join(gendb_dir, "title/title.off"));
        title_dat.open(path_join(gendb_dir, "title/title.dat"));
        mc_note_off.open(path_join(gendb_dir, "movie_companies/note.off"));
        mc_note_dat.open(path_join(gendb_dir, "movie_companies/note.dat"));
        mc_company_type_id.open(path_join(gendb_dir, "movie_companies/company_type_id.bin"));
        mc_movie_id_off_idx.open(path_join(gendb_dir, "_idx/movie_companies__movie_id__offsets.bin"));
        mi_info_type_id.open(path_join(gendb_dir, "movie_info_idx/info_type_id.bin"));
        mi_movie_id_off_idx.open(path_join(gendb_dir, "_idx/movie_info_idx__movie_id__offsets.bin"));
        title_prod_year.open(path_join(gendb_dir, "title/production_year.bin"));

        // Hint kernel: random access for varlen .dat (chase pointers).
        mc_note_dat.advise_random();
        title_dat.advise_random();
        // Sequential for FK arrays/offset indexes is fine (default).
        // Prefetch fact columns.
        mc_movie_id_off_idx.prefetch();
        mi_movie_id_off_idx.prefetch();
        mc_company_type_id.prefetch();
        mi_info_type_id.prefetch();
        title_prod_year.prefetch();
    }

    // ---- Resolve dimension scalars ----
    const size_t ct_rows = ct_kind_off.count - 1;
    const size_t it_rows = it_info_off.count - 1;

    int32_t target_ct_id = resolve_dense_pk_varlen(ct_kind_off.data, ct_kind_dat.data,
                                                  ct_rows, p_ct_kind.data(), p_ct_kind.size());
    int32_t target_it_id = resolve_dense_pk_varlen(it_info_off.data, it_info_dat.data,
                                                  it_rows, p_it_info.data(), p_it_info.size());

    const size_t n_title = title_off.count - 1;  // 2,528,312

    ThreadAgg global;

    if (target_ct_id < 0 || target_it_id < 0) {
        // No matches possible.
    } else {
        // ---- Parallel main scan over title.id range with morsels ----
        const int32_t* mi_off = mi_movie_id_off_idx.data;
        const int32_t* mc_off = mc_movie_id_off_idx.data;
        const int32_t* mi_it  = mi_info_type_id.data;
        const int32_t* mc_ct  = mc_company_type_id.data;
        const int64_t* mc_note_o = mc_note_off.data;
        const char*    mc_note_d = mc_note_dat.data;
        const int64_t* t_title_o = title_off.data;
        const char*    t_title_d = title_dat.data;
        const int32_t* t_year    = title_prod_year.data;

        const char* neg = p_neg_pat.data();   const size_t neg_len = p_neg_pat.size();
        const char* pos1 = p_pos_pat1.data(); const size_t pos1_len = p_pos_pat1.size();
        const char* pos2 = p_pos_pat2.data(); const size_t pos2_len = p_pos_pat2.size();
        const size_t min_pos_len = std::min(pos1_len, pos2_len);
        const int32_t target_it = target_it_id;
        const int32_t target_ct = target_ct_id;

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 12;
        const size_t MORSEL = 100000;
        const size_t total_morsels = (n_title + MORSEL - 1) / MORSEL;
        std::atomic<size_t> next_morsel{0};
        std::vector<ThreadAgg> per_thread(hw);
        std::vector<std::thread> threads;
        threads.reserve(hw);

        GENDB_PHASE_MS("main_scan", main_ms);
        (void)main_ms;

        for (unsigned t = 0; t < hw; ++t) {
            threads.emplace_back([&, t]() {
                ThreadAgg local;
                while (true) {
                    size_t m = next_morsel.fetch_add(1, std::memory_order_relaxed);
                    if (m >= total_morsels) break;
                    size_t lo_v = m * MORSEL + 1;                 // title.id starts at 1
                    size_t hi_v = std::min((size_t)n_title, lo_v + MORSEL - 1);

                    for (size_t v = lo_v; v <= hi_v; ++v) {
                        // Step 1: mi_idx semi-join (top-250 selective)
                        int32_t mi_lo = mi_off[v];
                        int32_t mi_hi = mi_off[v + 1];
                        bool mi_match = false;
                        for (int32_t r = mi_lo; r < mi_hi; ++r) {
                            if (mi_it[r] == target_it) { mi_match = true; break; }
                        }
                        if (!mi_match) continue;

                        // Step 2: mc inner join + filters
                        int32_t mc_lo = mc_off[v];
                        int32_t mc_hi = mc_off[v + 1];
                        if (mc_lo == mc_hi) continue;

                        // We may have multiple surviving mc rows for this title;
                        // each contributes a candidate for MIN(mc.note).
                        bool title_has_survivor = false;

                        for (int32_t r = mc_lo; r < mc_hi; ++r) {
                            if (mc_ct[r] != target_ct) continue;
                            int64_t no_lo = mc_note_o[r];
                            int64_t no_hi = mc_note_o[r + 1];
                            size_t  no_len = (size_t)(no_hi - no_lo);
                            if (no_len < min_pos_len) continue;  // empty (NULL) or too short for positive LIKE
                            const char* slice = mc_note_d + no_lo;

                            // Positive OR: '%(co-production)%' OR '%(presents)%'
                            bool pos = slice_contains(slice, no_len, pos1, pos1_len)
                                    || slice_contains(slice, no_len, pos2, pos2_len);
                            if (!pos) continue;

                            // Negative LIKE: NOT '%(as Metro-Goldwyn-Mayer Pictures)%'
                            if (slice_contains(slice, no_len, neg, neg_len)) continue;

                            // Survivor: update MIN(mc.note)
                            std::string_view nv(slice, no_len);
                            if (!local.has_note || nv < local.min_note) {
                                local.has_note = true;
                                local.min_note = nv;
                            }
                            title_has_survivor = true;
                        }

                        if (!title_has_survivor) continue;

                        // Project t.title (varlen) for this title
                        int64_t t_lo = t_title_o[v - 1];
                        int64_t t_hi = t_title_o[v];
                        size_t  t_len = (size_t)(t_hi - t_lo);
                        std::string_view tv(t_title_d + t_lo, t_len);
                        if (!local.has_title || tv < local.min_title) {
                            local.has_title = true;
                            local.min_title = tv;
                        }

                        // Project t.production_year (skip NULL = INT32_MIN)
                        int32_t yr = t_year[v - 1];
                        if (yr != INT32_MIN) {
                            if (!local.has_year || yr < local.min_year) {
                                local.has_year = true;
                                local.min_year = yr;
                            }
                        }
                    }
                }
                per_thread[t] = local;
            });
        }
        for (auto& th : threads) th.join();

        // Reduce thread-local mins
        for (auto& l : per_thread) {
            if (l.has_note && (!global.has_note || l.min_note < global.min_note)) {
                global.has_note = true; global.min_note = l.min_note;
            }
            if (l.has_title && (!global.has_title || l.min_title < global.min_title)) {
                global.has_title = true; global.min_title = l.min_title;
            }
            if (l.has_year && (!global.has_year || l.min_year < global.min_year)) {
                global.has_year = true; global.min_year = l.min_year;
            }
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = path_join(results_dir, "Q1a.csv");
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
            return 2;
        }
        std::fputs("production_note,movie_title,movie_year\n", f);

        auto write_csv_field = [&](std::string_view s) {
            // Quote if contains comma, quote, CR, or LF.
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

        if (global.has_note) write_csv_field(global.min_note);
        std::fputc(',', f);
        if (global.has_title) write_csv_field(global.min_title);
        std::fputc(',', f);
        if (global.has_year) std::fprintf(f, "%d", global.min_year);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
