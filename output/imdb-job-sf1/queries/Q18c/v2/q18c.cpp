// Q18c
// SELECT MIN(mi.info), MIN(mi_idx.info), MIN(t.title)
// FROM cast_info ci, info_type it1, info_type it2, movie_info mi,
//      movie_info_idx mi_idx, name n, title t
// WHERE ci.note IN (...) AND it1.info='genres' AND it2.info='votes'
//       AND mi.info IN (...) AND n.gender='m'
//       AND joins on movie_id and person_id and info_type_id

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <mutex>

#include "timing_utils.h"
#include "cli_params.h"

// ---------- mmap helper ----------
static const void* mmap_file(const std::string& path, size_t* out_size = nullptr) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", path.c_str());
        std::exit(2);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        std::fprintf(stderr, "fstat failed: %s\n", path.c_str());
        std::exit(2);
    }
    size_t sz = (size_t)st.st_size;
    void* p = mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed: %s\n", path.c_str());
        std::exit(2);
    }
    close(fd);
    if (out_size) *out_size = sz;
    return p;
}

// ---------- MIN reduction helper ----------
static inline void update_min(std::string& cur, bool& have, std::string_view v) {
    if (!have) { cur.assign(v.data(), v.size()); have = true; }
    else if (v < std::string_view(cur)) { cur.assign(v.data(), v.size()); }
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [--gender_eq m] [--info_eq votes] [--info_eq_2 genres]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    std::string gender_eq = gendb::parse_string_arg(argc, argv, "--gender_eq", "m");
    std::string info_eq    = gendb::parse_string_arg(argc, argv, "--info_eq", "votes");
    std::string info_eq_2  = gendb::parse_string_arg(argc, argv, "--info_eq_2", "genres");

    // -------- Load metadata --------
    const uint64_t TITLE_N = 2528312;
    const uint64_t CI_N    = 36244344;
    const uint64_t MI_N    = 14835720;
    const uint64_t MII_N   = 1380035;
    const uint64_t NAME_N  = 4167491;
    const uint64_t IT_N    = 113;
    const int32_t MAX_MOVIE_ID = 2528312;

    // -------- mmap files --------
    const int32_t* off_mi    = nullptr;
    const int32_t* off_mii   = nullptr;
    const int32_t* off_ci    = nullptr;
    const int32_t* mi_info_type_id  = nullptr;
    const int32_t* mii_info_type_id = nullptr;
    const int32_t* ci_person_id     = nullptr;
    const uint64_t* mi_info_off  = nullptr;
    const char*     mi_info_dat  = nullptr;
    const uint64_t* mii_info_off = nullptr;
    const char*     mii_info_dat = nullptr;
    const uint64_t* ci_note_off  = nullptr;
    const char*     ci_note_dat  = nullptr;
    const uint64_t* title_off    = nullptr;
    const char*     title_dat    = nullptr;
    const uint64_t* it_info_off  = nullptr;
    const char*     it_info_dat  = nullptr;
    const int32_t*  it_id        = nullptr;
    const uint8_t*  name_gender  = nullptr;
    const uint64_t* gdict_off    = nullptr;
    const char*     gdict_dat    = nullptr;
    size_t gdict_dat_sz = 0;
    size_t gdict_off_sz = 0;

    {
        GENDB_PHASE("data_loading");
        off_mi  = (const int32_t*)mmap_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        off_mii = (const int32_t*)mmap_file(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        off_ci  = (const int32_t*)mmap_file(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");

        mi_info_type_id  = (const int32_t*)mmap_file(gendb_dir + "/movie_info/info_type_id.bin");
        mii_info_type_id = (const int32_t*)mmap_file(gendb_dir + "/movie_info_idx/info_type_id.bin");
        ci_person_id     = (const int32_t*)mmap_file(gendb_dir + "/cast_info/person_id.bin");

        mi_info_off  = (const uint64_t*)mmap_file(gendb_dir + "/movie_info/info.off");
        mi_info_dat  = (const char*)    mmap_file(gendb_dir + "/movie_info/info.dat");
        mii_info_off = (const uint64_t*)mmap_file(gendb_dir + "/movie_info_idx/info.off");
        mii_info_dat = (const char*)    mmap_file(gendb_dir + "/movie_info_idx/info.dat");
        ci_note_off  = (const uint64_t*)mmap_file(gendb_dir + "/cast_info/note.off");
        ci_note_dat  = (const char*)    mmap_file(gendb_dir + "/cast_info/note.dat");

        title_off = (const uint64_t*)mmap_file(gendb_dir + "/title/title.off");
        title_dat = (const char*)    mmap_file(gendb_dir + "/title/title.dat");

        it_info_off = (const uint64_t*)mmap_file(gendb_dir + "/info_type/info.off");
        it_info_dat = (const char*)    mmap_file(gendb_dir + "/info_type/info.dat");
        it_id       = (const int32_t*) mmap_file(gendb_dir + "/info_type/id.bin");

        name_gender = (const uint8_t*) mmap_file(gendb_dir + "/name/gender.bin");
        gdict_off   = (const uint64_t*)mmap_file(gendb_dir + "/name/gender.dict.off", &gdict_off_sz);
        gdict_dat   = (const char*)    mmap_file(gendb_dir + "/name/gender.dict.dat", &gdict_dat_sz);
    }

    (void)CI_N; (void)MI_N; (void)MII_N; (void)NAME_N; (void)MAX_MOVIE_ID;

    // -------- Resolve dimension literals --------
    int32_t it1_id = -1; // genres
    int32_t it2_id = -1; // votes
    {
        GENDB_PHASE("dim_resolve");
        std::string_view target1(info_eq_2); // genres
        std::string_view target2(info_eq);   // votes
        for (uint64_t r = 0; r < IT_N; ++r) {
            std::string_view s(it_info_dat + it_info_off[r], it_info_off[r+1] - it_info_off[r]);
            if (s == target1) it1_id = it_id[r];
            if (s == target2) it2_id = it_id[r];
        }
        if (it1_id < 0 || it2_id < 0) {
            std::fprintf(stderr, "Failed to resolve info_type literals: it1=%d it2=%d\n", it1_id, it2_id);
            return 2;
        }
    }

    // -------- Resolve gender dict code --------
    uint8_t gender_code = 255;
    {
        GENDB_PHASE("gender_resolve");
        // Dict-encoded char1: data values are 1-based codes (code = dict_index + 1),
        // with code 0 reserved for NULL.
        uint64_t n_dict = (gdict_off_sz / 8) - 1;
        std::string_view target(gender_eq);
        for (uint64_t r = 0; r < n_dict; ++r) {
            std::string_view s(gdict_dat + gdict_off[r], gdict_off[r+1] - gdict_off[r]);
            if (s == target) { gender_code = (uint8_t)(r + 1); break; }
        }
        if (gender_code == 255) {
            std::fprintf(stderr, "Failed to resolve gender '%s'\n", gender_eq.c_str());
            return 2;
        }
    }

    // -------- Build name gender bitmap (name is dense 1..N -> row = id - 1) --------
    // Bitset indexed by name row position (== name.id - 1)
    std::vector<uint64_t> name_gender_bm;
    {
        GENDB_PHASE("build_gender_bitmap");
        size_t words = (NAME_N + 63) / 64;
        name_gender_bm.assign(words, 0);
        // Parallel scan
        const int T = (int)std::thread::hardware_concurrency();
        int nthr = T > 0 ? T : 1;
        std::vector<std::thread> threads;
        size_t chunk = (NAME_N + nthr - 1) / nthr;
        for (int t = 0; t < nthr; ++t) {
            size_t lo = (size_t)t * chunk;
            size_t hi = std::min(lo + chunk, (size_t)NAME_N);
            if (lo >= hi) continue;
            threads.emplace_back([&, lo, hi]() {
                for (size_t r = lo; r < hi; ++r) {
                    if (name_gender[r] == gender_code) {
                        __atomic_fetch_or(&name_gender_bm[r >> 6], (uint64_t)1 << (r & 63), __ATOMIC_RELAXED);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // -------- Filter sets --------
    // ci.note literals
    static const char* CI_NOTES[] = {
        "(writer)", "(head writer)", "(written by)", "(story)", "(story editor)"
    };
    std::unordered_set<std::string> ci_note_set;
    for (auto* s : CI_NOTES) ci_note_set.emplace(s);

    static const char* MI_GENRES[] = {
        "Horror", "Action", "Sci-Fi", "Thriller", "Crime", "War"
    };
    std::unordered_set<std::string> mi_info_set;
    for (auto* s : MI_GENRES) mi_info_set.emplace(s);

    // -------- Main scan (parallel over title id) --------
    struct LocalAgg {
        std::string min_mi;       // MIN(mi.info)  -> movie_budget
        std::string min_mii;      // MIN(mi_idx.info) -> movie_votes
        std::string min_title;    // MIN(t.title) -> movie_title
        bool have_mi = false;
        bool have_mii = false;
        bool have_title = false;
    };

    const int T = (int)std::thread::hardware_concurrency();
    int nthr = T > 0 ? T : 1;
    std::vector<LocalAgg> locals(nthr);

    {
        GENDB_PHASE("main_scan");

        // Use atomic morsel counter for dynamic load balancing
        std::atomic<uint64_t> next_morsel{0};
        const uint64_t MORSEL = 16384;
        // tid ranges from 1..TITLE_N inclusive
        uint64_t total_morsels = (TITLE_N + MORSEL - 1) / MORSEL;

        std::vector<std::thread> threads;
        for (int t = 0; t < nthr; ++t) {
            threads.emplace_back([&, t]() {
                LocalAgg& L = locals[t];
                while (true) {
                    uint64_t m = next_morsel.fetch_add(1, std::memory_order_relaxed);
                    if (m >= total_morsels) break;
                    uint64_t tid_lo = m * MORSEL + 1;
                    uint64_t tid_hi = std::min(tid_lo + MORSEL, (uint64_t)TITLE_N + 1);

                    for (uint64_t tid = tid_lo; tid < tid_hi; ++tid) {
                        // ----- Step 1: probe mi_idx CSR -----
                        int32_t mii_lo = off_mii[tid];
                        int32_t mii_hi = off_mii[tid + 1];
                        if (mii_lo == mii_hi) continue;

                        // Find candidates matching info_type_id == it2_id
                        // Track them by row positions in mi_idx
                        // Small ranges, just iterate; track count
                        int mii_match = 0;
                        // We don't need to store positions — we'll re-iterate below for MIN
                        for (int32_t r = mii_lo; r < mii_hi; ++r) {
                            if (mii_info_type_id[r] == it2_id) { mii_match++; }
                        }
                        if (mii_match == 0) continue;

                        // ----- Step 3: probe mi CSR -----
                        int32_t mi_lo = off_mi[tid];
                        int32_t mi_hi = off_mi[tid + 1];
                        if (mi_lo == mi_hi) continue;

                        // Local list of qualifying mi row positions (typically tiny)
                        int mi_match = 0;
                        // First quick check: any row with it1 + genre?
                        // Just iterate inline
                        // For MIN, we will re-iterate after ci probe passes.
                        // But it's cheaper to compute MIN here and remember whether any matched.
                        // We'll defer MIN update until after ci passes too.
                        // Use a small stack buffer of indices.
                        int32_t mi_rows[256];
                        int mi_rows_n = 0;
                        for (int32_t r = mi_lo; r < mi_hi; ++r) {
                            if (mi_info_type_id[r] != it1_id) continue;
                            std::string_view s(mi_info_dat + mi_info_off[r], mi_info_off[r+1] - mi_info_off[r]);
                            // lookup in 6-genre set
                            // small unordered_set with 6 entries — cheap
                            auto it = mi_info_set.find(std::string(s));
                            if (it == mi_info_set.end()) continue;
                            if (mi_rows_n < 256) mi_rows[mi_rows_n++] = r;
                            mi_match++;
                        }
                        if (mi_match == 0) continue;

                        // ----- Step 5: probe ci CSR with note + gender semijoin -----
                        int32_t ci_lo = off_ci[tid];
                        int32_t ci_hi = off_ci[tid + 1];
                        if (ci_lo == ci_hi) continue;

                        bool ci_ok = false;
                        for (int32_t r = ci_lo; r < ci_hi; ++r) {
                            uint64_t n_lo = ci_note_off[r];
                            uint64_t n_hi = ci_note_off[r + 1];
                            if (n_lo == n_hi) continue; // NULL note
                            std::string_view note(ci_note_dat + n_lo, n_hi - n_lo);
                            auto itn = ci_note_set.find(std::string(note));
                            if (itn == ci_note_set.end()) continue;
                            int32_t pid = ci_person_id[r];
                            if (pid <= 0 || (uint64_t)pid > NAME_N) continue;
                            // name row position == pid - 1 (dense)
                            size_t pos = (size_t)pid - 1;
                            if ((name_gender_bm[pos >> 6] >> (pos & 63)) & 1ull) {
                                ci_ok = true;
                                break;
                            }
                        }
                        if (!ci_ok) continue;

                        // ----- Aggregate: MIN(mi.info), MIN(mi_idx.info), MIN(t.title) -----
                        // mi.info — over qualifying mi rows
                        for (int i = 0; i < mi_rows_n; ++i) {
                            int32_t r = mi_rows[i];
                            std::string_view s(mi_info_dat + mi_info_off[r], mi_info_off[r+1] - mi_info_off[r]);
                            update_min(L.min_mi, L.have_mi, s);
                        }
                        // If mi_match exceeded buffer (extremely unlikely), re-scan
                        if (mi_match > mi_rows_n) {
                            for (int32_t r = mi_lo; r < mi_hi; ++r) {
                                if (mi_info_type_id[r] != it1_id) continue;
                                std::string_view s(mi_info_dat + mi_info_off[r], mi_info_off[r+1] - mi_info_off[r]);
                                if (mi_info_set.find(std::string(s)) == mi_info_set.end()) continue;
                                update_min(L.min_mi, L.have_mi, s);
                            }
                        }

                        // mi_idx.info — re-iterate over mii range and update MIN
                        for (int32_t r = mii_lo; r < mii_hi; ++r) {
                            if (mii_info_type_id[r] != it2_id) continue;
                            std::string_view s(mii_info_dat + mii_info_off[r], mii_info_off[r+1] - mii_info_off[r]);
                            update_min(L.min_mii, L.have_mii, s);
                        }

                        // t.title — title is dense 1..N, so row = tid - 1
                        size_t trow = (size_t)tid - 1;
                        std::string_view ts(title_dat + title_off[trow], title_off[trow+1] - title_off[trow]);
                        update_min(L.min_title, L.have_title, ts);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // -------- Reduce locals --------
    std::string min_mi, min_mii, min_title;
    bool have_mi = false, have_mii = false, have_title = false;
    for (auto& L : locals) {
        if (L.have_mi)    update_min(min_mi,    have_mi,    L.min_mi);
        if (L.have_mii)   update_min(min_mii,   have_mii,   L.min_mii);
        if (L.have_title) update_min(min_title, have_title, L.min_title);
    }

    // -------- Output --------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q18c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "failed to open %s\n", out_path.c_str());
            return 3;
        }
        std::fprintf(f, "movie_budget,movie_votes,movie_title\n");
        // CSV write — values are unlikely to contain commas but to be safe nothing
        // Empty if no result
        std::fprintf(f, "%s,%s,%s\n",
                     have_mi ? min_mi.c_str() : "",
                     have_mii ? min_mii.c_str() : "",
                     have_title ? min_title.c_str() : "");
        std::fclose(f);
    }

    return 0;
}
