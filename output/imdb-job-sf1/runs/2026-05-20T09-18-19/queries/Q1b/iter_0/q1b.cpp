// Q1b: MIN(mc.note), MIN(t.title), MIN(t.production_year)
// FROM company_type ct, info_type it, movie_companies mc, movie_info_idx mi_idx, title t
// WHERE ct.kind='production companies', it.info='bottom 10 rank',
//       mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%',
//       t.production_year BETWEEN 2005 AND 2010
// Three INDEPENDENT mins.

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <climits>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;

static const char* MGM_NEEDLE = "(as Metro-Goldwyn-Mayer Pictures)";
static const size_t MGM_NEEDLE_LEN = 33; // length of needle above

// Lexicographic comparison of byte strings; returns negative/zero/positive.
static inline int lex_cmp(const uint8_t* a, int32_t alen,
                          const uint8_t* b, int32_t blen) {
    int32_t n = (alen < blen) ? alen : blen;
    int c = std::memcmp(a, b, (size_t)n);
    if (c != 0) return c;
    return alen - blen;
}

// Boyer-Moore-Horspool-ish? Just use memmem since glibc provides it.
// Fallback: simple loop. Both safe.
static inline bool contains_needle(const uint8_t* hay, int32_t hlen,
                                   const char* nd, size_t nlen) {
    if ((size_t)hlen < nlen) return false;
    return memmem(hay, (size_t)hlen, nd, nlen) != nullptr;
}

struct LocalMin {
    bool note_set;
    const uint8_t* note_ptr;
    int32_t note_len;
    bool title_set;
    const uint8_t* title_ptr;
    int32_t title_len;
    bool year_set;
    int32_t year;
    // Padding to avoid false sharing
    char pad[24];

    LocalMin() : note_set(false), note_ptr(nullptr), note_len(0),
                 title_set(false), title_ptr(nullptr), title_len(0),
                 year_set(false), year(INT32_MAX) {}
};

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];

    MmapColumn<int32_t> ct_id;
    MmapColumn<uint8_t> ct_kind_dat;
    MmapColumn<int64_t> ct_kind_off;
    MmapColumn<int32_t> it_id;
    MmapColumn<uint8_t> it_info_dat;
    MmapColumn<int64_t> it_info_off;
    MmapColumn<int32_t> title_year;
    MmapColumn<int64_t> title_off;
    MmapColumn<uint8_t> title_dat;
    MmapColumn<int32_t> mc_off_idx;
    MmapColumn<int32_t> mi_off_idx;
    MmapColumn<int32_t> mc_ct;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<uint8_t> mc_note_dat;
    MmapColumn<int32_t> mi_it;

    {
        GENDB_PHASE("data_loading");
        ct_id.open(gendb + "/company_type/id.bin");
        ct_kind_dat.open(gendb + "/company_type/kind.dat");
        ct_kind_off.open(gendb + "/company_type/kind.off");
        it_id.open(gendb + "/info_type/id.bin");
        it_info_dat.open(gendb + "/info_type/info.dat");
        it_info_off.open(gendb + "/info_type/info.off");
        title_year.open(gendb + "/title/production_year.bin");
        title_off.open(gendb + "/title/title.off");
        title_dat.open(gendb + "/title/title.dat");
        mc_off_idx.open(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off_idx.open(gendb + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mc_ct.open(gendb + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gendb + "/movie_companies/note.off");
        mc_note_dat.open(gendb + "/movie_companies/note.dat");
        mi_it.open(gendb + "/movie_info_idx/info_type_id.bin");

        // Random-access indexes (offsets, mc_ct, mi_it, note_off accessed via gather)
        mc_off_idx.advise_random();
        mi_off_idx.advise_random();
        mc_ct.advise_random();
        mi_it.advise_random();
        mc_note_off.advise_random();
        mc_note_dat.advise_random();
        title_off.advise_random();
        title_dat.advise_random();
        // year is sequentially scanned
        title_year.advise_sequential();

        mmap_prefetch_all(mc_off_idx, mi_off_idx, mc_ct, mi_it,
                          mc_note_off, mc_note_dat, title_year);
    }

    int32_t target_ct = -1, target_it = -1;
    {
        GENDB_PHASE("dim_resolve");
        const char* needle_ct = "production companies";
        size_t nclen = std::strlen(needle_ct);
        for (size_t i = 0; i < ct_id.count; i++) {
            int64_t s = ct_kind_off[i], e = ct_kind_off[i + 1];
            if ((int64_t)(e - s) == (int64_t)nclen &&
                std::memcmp(ct_kind_dat.data + s, needle_ct, nclen) == 0) {
                target_ct = ct_id[i];
                break;
            }
        }
        const char* needle_it = "bottom 10 rank";
        size_t nilen = std::strlen(needle_it);
        for (size_t i = 0; i < it_id.count; i++) {
            int64_t s = it_info_off[i], e = it_info_off[i + 1];
            if ((int64_t)(e - s) == (int64_t)nilen &&
                std::memcmp(it_info_dat.data + s, needle_it, nilen) == 0) {
                target_it = it_id[i];
                break;
            }
        }
    }

    bool have_targets = (target_ct >= 0) && (target_it >= 0);

    LocalMin global;
    if (have_targets) {
        size_t num_titles = title_year.count;
        int num_threads = (int)std::thread::hardware_concurrency();
        if (num_threads <= 0) num_threads = 1;
        if (num_threads > 24) num_threads = 24;

        std::vector<LocalMin> locals(num_threads);
        std::atomic<size_t> next_morsel{0};
        const size_t MORSEL = 100000;

        auto worker = [&](int tid) {
            LocalMin& lm = locals[tid];
            const int32_t tgt_ct = target_ct;
            const int32_t tgt_it = target_it;
            const int32_t* year_arr = title_year.data;
            const int32_t* mc_off_arr = mc_off_idx.data;
            const int32_t* mi_off_arr = mi_off_idx.data;
            const int32_t* mc_ct_arr = mc_ct.data;
            const int32_t* mi_it_arr = mi_it.data;
            const int64_t* mc_note_off_arr = mc_note_off.data;
            const uint8_t* mc_note_dat_ptr = mc_note_dat.data;
            const int64_t* t_off_arr = title_off.data;
            const uint8_t* t_dat_ptr = title_dat.data;

            while (true) {
                size_t start = next_morsel.fetch_add(MORSEL);
                if (start >= num_titles) break;
                size_t end = std::min(start + MORSEL, num_titles);
                for (size_t i = start; i < end; i++) {
                    int32_t y = year_arr[i];
                    if (y < 2005 || y > 2010) continue; // INT32_MIN is < 2005
                    int32_t v = (int32_t)(i + 1); // title.id

                    // mi_idx existence check first (very selective)
                    int32_t mi_lo = mi_off_arr[v];
                    int32_t mi_hi = mi_off_arr[v + 1];
                    bool found_it = false;
                    for (int32_t r = mi_lo; r < mi_hi; r++) {
                        if (mi_it_arr[r] == tgt_it) { found_it = true; break; }
                    }
                    if (!found_it) continue;

                    // mc scan
                    int32_t mc_lo = mc_off_arr[v];
                    int32_t mc_hi = mc_off_arr[v + 1];
                    bool any_pass = false;
                    for (int32_t r = mc_lo; r < mc_hi; r++) {
                        if (mc_ct_arr[r] != tgt_ct) continue;
                        int64_t ns = mc_note_off_arr[r];
                        int64_t ne = mc_note_off_arr[r + 1];
                        int32_t nlen = (int32_t)(ne - ns);
                        if (nlen <= 0) continue; // empty note fails per guide
                        const uint8_t* nptr = mc_note_dat_ptr + ns;
                        if (contains_needle(nptr, nlen, MGM_NEEDLE, MGM_NEEDLE_LEN)) continue;
                        any_pass = true;
                        if (!lm.note_set ||
                            lex_cmp(nptr, nlen, lm.note_ptr, lm.note_len) < 0) {
                            lm.note_set = true;
                            lm.note_ptr = nptr;
                            lm.note_len = nlen;
                        }
                    }
                    if (any_pass) {
                        int64_t ts = t_off_arr[i];
                        int64_t te = t_off_arr[i + 1];
                        int32_t tlen = (int32_t)(te - ts);
                        const uint8_t* tptr = t_dat_ptr + ts;
                        if (!lm.title_set ||
                            lex_cmp(tptr, tlen, lm.title_ptr, lm.title_len) < 0) {
                            lm.title_set = true;
                            lm.title_ptr = tptr;
                            lm.title_len = tlen;
                        }
                        if (!lm.year_set || y < lm.year) {
                            lm.year_set = true;
                            lm.year = y;
                        }
                    }
                }
            }
        };

        {
            GENDB_PHASE("main_scan");
            std::vector<std::thread> threads;
            threads.reserve(num_threads);
            for (int t = 0; t < num_threads; t++) threads.emplace_back(worker, t);
            for (auto& th : threads) th.join();
        }

        for (auto& lm : locals) {
            if (lm.note_set && (!global.note_set ||
                lex_cmp(lm.note_ptr, lm.note_len, global.note_ptr, global.note_len) < 0)) {
                global.note_set = true;
                global.note_ptr = lm.note_ptr;
                global.note_len = lm.note_len;
            }
            if (lm.title_set && (!global.title_set ||
                lex_cmp(lm.title_ptr, lm.title_len, global.title_ptr, global.title_len) < 0)) {
                global.title_set = true;
                global.title_ptr = lm.title_ptr;
                global.title_len = lm.title_len;
            }
            if (lm.year_set && (!global.year_set || lm.year < global.year)) {
                global.year_set = true;
                global.year = lm.year;
            }
        }
    }

    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q1b.csv";
        FILE* f = std::fopen(outpath.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Cannot open %s\n", outpath.c_str()); return 1; }
        std::fprintf(f, "production_note,movie_title,movie_year\n");
        auto write_csv_field = [&](const uint8_t* p, int32_t l) {
            bool needs_quote = false;
            for (int32_t i = 0; i < l; i++) {
                uint8_t c = p[i];
                if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                    needs_quote = true;
                    break;
                }
            }
            if (!needs_quote) {
                std::fwrite(p, 1, (size_t)l, f);
            } else {
                std::fputc('"', f);
                for (int32_t i = 0; i < l; i++) {
                    if (p[i] == '"') std::fputc('"', f);
                    std::fputc(p[i], f);
                }
                std::fputc('"', f);
            }
        };
        if (global.note_set) write_csv_field(global.note_ptr, global.note_len);
        std::fputc(',', f);
        if (global.title_set) write_csv_field(global.title_ptr, global.title_len);
        std::fputc(',', f);
        if (global.year_set) std::fprintf(f, "%d", global.year);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
