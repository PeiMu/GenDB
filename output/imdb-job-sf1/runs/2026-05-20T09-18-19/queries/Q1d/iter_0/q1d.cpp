// Q1d implementation: MIN(mc.note), MIN(t.title), MIN(t.production_year)
// over title × movie_companies × movie_info_idx with filters on
// company_type.kind='production companies', info_type.info='bottom 10 rank',
// mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%', and t.production_year > 2000.
//
// Strategy (from plan.json):
//   1. Resolve target_ct_id from company_type.kind (4 rows).
//   2. Resolve target_it_id from info_type.info (113 rows).
//   3. Drive scan over title.production_year; for each surviving title
//      (year > 2000), probe mi_idx range via offsets-only index first
//      (rare 'bottom 10 rank' short-circuits), then probe mc range,
//      filter by ct and NOT LIKE on note, update per-thread MIN accumulators.
//   4. Merge and emit CSV.

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;

// ---------------------------------------------------------------------------
// memmem fallback (POSIX/GNU has it; ensure availability)
// ---------------------------------------------------------------------------
static inline const void* gendb_memmem(const void* haystack, size_t hlen,
                                       const void* needle, size_t nlen) {
    if (nlen == 0) return haystack;
    if (hlen < nlen) return nullptr;
    return memmem(haystack, hlen, needle, nlen);
}

// ---------------------------------------------------------------------------
// Varlen helpers: column has .off (int64_t[N+1]) and .dat (uint8_t[total])
// Slice for row i: ptr = dat + off[i], len = off[i+1] - off[i]
// ---------------------------------------------------------------------------
struct VarlenCol {
    MmapColumn<int64_t> off;
    MmapColumn<uint8_t> dat;
    void open(const std::string& base) {
        off.open(base + ".off");
        dat.open(base + ".dat");
    }
    inline const uint8_t* ptr(size_t i) const { return dat.data + off.data[i]; }
    inline size_t len(size_t i) const { return (size_t)(off.data[i+1] - off.data[i]); }
};

// Lexicographic compare for varlen slices: returns <0, 0, >0.
static inline int varlen_cmp(const uint8_t* a, size_t la,
                             const uint8_t* b, size_t lb) {
    size_t m = la < lb ? la : lb;
    int c = std::memcmp(a, b, m);
    if (c != 0) return c;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

// ---------------------------------------------------------------------------
// Resolve target id by scanning a small dimension's varlen column.
// Returns row index (== dense identity id) of the matching row, or -1.
// ---------------------------------------------------------------------------
static int32_t resolve_id(const VarlenCol& v, size_t n, const char* target) {
    size_t tlen = std::strlen(target);
    for (size_t i = 0; i < n; ++i) {
        if (v.len(i) == tlen && std::memcmp(v.ptr(i), target, tlen) == 0) {
            return (int32_t)(i + 1); // dense identity: id = index + 1
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Per-thread MIN accumulator (for varlen, store row index of best).
// ---------------------------------------------------------------------------
struct MinAccum {
    bool     have_note;
    const uint8_t* note_ptr;
    size_t   note_len;

    bool     have_title;
    const uint8_t* title_ptr;
    size_t   title_len;

    int32_t  min_year;

    MinAccum() : have_note(false), note_ptr(nullptr), note_len(0),
                 have_title(false), title_ptr(nullptr), title_len(0),
                 min_year(INT32_MAX) {}

    inline void update_note(const uint8_t* p, size_t l) {
        if (!have_note || varlen_cmp(p, l, note_ptr, note_len) < 0) {
            note_ptr = p; note_len = l; have_note = true;
        }
    }
    inline void update_title(const uint8_t* p, size_t l) {
        if (!have_title || varlen_cmp(p, l, title_ptr, title_len) < 0) {
            title_ptr = p; title_len = l; have_title = true;
        }
    }
    inline void update_year(int32_t y) {
        if (y < min_year) min_year = y;
    }
    void merge(const MinAccum& o) {
        if (o.have_note) update_note(o.note_ptr, o.note_len);
        if (o.have_title) update_title(o.title_ptr, o.title_len);
        if (o.min_year < min_year) min_year = o.min_year;
    }
};

// CSV field escape: quote if contains comma, quote, CR or LF.
static void csv_emit(FILE* f, const uint8_t* p, size_t n) {
    bool need_quote = false;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        std::fwrite(p, 1, n, f);
        return;
    }
    std::fputc('"', f);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c == '"') std::fputc('"', f);
        std::fputc((int)c, f);
    }
    std::fputc('"', f);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    GENDB_PHASE("total");

    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    (void)argc; (void)argv;

    // ------------------------------------------------------------------
    // Open all data files (data_loading phase).
    // ------------------------------------------------------------------
    VarlenCol ct_kind;
    VarlenCol it_info;
    MmapColumn<int32_t> mc_movie_id;       // unused (drive via offsets index)
    MmapColumn<int32_t> mc_company_type_id;
    VarlenCol mc_note;
    MmapColumn<int32_t> mi_movie_id;       // unused (drive via offsets index)
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int32_t> t_year;
    VarlenCol t_title;
    MmapColumn<int32_t> mc_off;
    MmapColumn<int32_t> mi_off;

    int32_t target_ct_id = -1;
    int32_t target_it_id = -1;
    size_t  n_titles = 0;

    {
        GENDB_PHASE("data_loading");
        ct_kind.open(gendb_dir + "/company_type/kind");
        it_info.open(gendb_dir + "/info_type/info");

        mc_company_type_id.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note.open(gendb_dir + "/movie_companies/note");
        mi_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");

        t_year.open(gendb_dir + "/title/production_year.bin");
        t_title.open(gendb_dir + "/title/title");

        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");

        // Prefetch large columns
        mmap_prefetch_all(mc_company_type_id, mi_info_type_id, t_year, mc_off, mi_off);

        n_titles = t_year.count;

        // Resolve target ids (scan tiny dimension tables, 4 + 113 rows).
        target_ct_id = resolve_id(ct_kind, ct_kind.off.count > 0 ? ct_kind.off.count - 1 : 0,
                                  "production companies");
        target_it_id = resolve_id(it_info, it_info.off.count > 0 ? it_info.off.count - 1 : 0,
                                  "bottom 10 rank");
    }

    if (target_ct_id < 0 || target_it_id < 0) {
        std::fprintf(stderr, "Failed to resolve target ids: ct=%d it=%d\n",
                     target_ct_id, target_it_id);
        // Still write an empty result file with header.
        mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q1d.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (f) {
            std::fputs("production_note,movie_title,movie_year\n", f);
            std::fclose(f);
        }
        return 0;
    }

    static const char NEEDLE[] = "(as Metro-Goldwyn-Mayer Pictures)";
    static const size_t NEEDLE_LEN = sizeof(NEEDLE) - 1;

    // ------------------------------------------------------------------
    // Parallel main scan over title rows.
    // ------------------------------------------------------------------
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;
    if (nthreads > 12) nthreads = 12;

    std::vector<MinAccum> partials(nthreads);

    // Snapshot raw pointers for hot loop.
    const int32_t* yr      = t_year.data;
    const int32_t* mc_ct   = mc_company_type_id.data;
    const int32_t* mi_it   = mi_info_type_id.data;
    const int32_t* mc_o    = mc_off.data;
    const int32_t* mi_o    = mi_off.data;
    const uint8_t* mc_note_dat = mc_note.dat.data;
    const int64_t* mc_note_off = mc_note.off.data;
    const uint8_t* t_title_dat = t_title.dat.data;
    const int64_t* t_title_off = t_title.off.data;

    const int32_t TCT = target_ct_id;
    const int32_t TIT = target_it_id;
    const size_t mc_off_count = mc_off.count;
    const size_t mi_off_count = mi_off.count;

    {
        GENDB_PHASE("main_scan");

        std::vector<std::thread> threads;
        threads.reserve(nthreads);

        size_t chunk = (n_titles + nthreads - 1) / nthreads;

        for (unsigned tid = 0; tid < nthreads; ++tid) {
            size_t lo = (size_t)tid * chunk;
            size_t hi = std::min(lo + chunk, n_titles);
            if (lo >= hi) continue;
            threads.emplace_back([&, tid, lo, hi]() {
                MinAccum& acc = partials[tid];
                for (size_t i = lo; i < hi; ++i) {
                    int32_t y = yr[i];
                    if (y == INT32_MIN || y <= 2000) continue;

                    // movie_id = title row index + 1 (dense identity).
                    int32_t mid = (int32_t)(i + 1);
                    if ((size_t)mid + 1 > mi_off_count) continue;

                    // 1) Probe mi_idx range first (rare info_type short-circuit).
                    int32_t mi_lo = mi_o[mid];
                    int32_t mi_hi = mi_o[mid + 1];
                    bool mi_match = false;
                    for (int32_t s = mi_lo; s < mi_hi; ++s) {
                        if (mi_it[s] == TIT) { mi_match = true; break; }
                    }
                    if (!mi_match) continue;

                    // 2) Probe mc range; filter ct, note NOT NULL, NOT LIKE.
                    if ((size_t)mid + 1 > mc_off_count) continue;
                    int32_t mc_lo = mc_o[mid];
                    int32_t mc_hi = mc_o[mid + 1];
                    for (int32_t r = mc_lo; r < mc_hi; ++r) {
                        if (mc_ct[r] != TCT) continue;
                        size_t nlen = (size_t)(mc_note_off[r + 1] - mc_note_off[r]);
                        if (nlen == 0) continue; // NULL note fails NOT LIKE
                        const uint8_t* nptr = mc_note_dat + mc_note_off[r];
                        if (gendb_memmem(nptr, nlen, NEEDLE, NEEDLE_LEN) != nullptr) continue;

                        // Survivor: update MIN accumulators.
                        acc.update_note(nptr, nlen);

                        size_t tlen = (size_t)(t_title_off[i + 1] - t_title_off[i]);
                        const uint8_t* tptr = t_title_dat + t_title_off[i];
                        acc.update_title(tptr, tlen);

                        acc.update_year(y);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // Merge partial accumulators.
    MinAccum final_acc;
    for (auto& p : partials) final_acc.merge(p);

    // ------------------------------------------------------------------
    // Output CSV.
    // ------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q1d.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Failed to open %s\n", out_path.c_str());
            return 1;
        }
        std::fputs("production_note,movie_title,movie_year\n", f);

        if (final_acc.have_note) {
            csv_emit(f, final_acc.note_ptr, final_acc.note_len);
        }
        std::fputc(',', f);
        if (final_acc.have_title) {
            csv_emit(f, final_acc.title_ptr, final_acc.title_len);
        }
        std::fputc(',', f);
        if (final_acc.min_year != INT32_MAX) {
            std::fprintf(f, "%d", final_acc.min_year);
        }
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
