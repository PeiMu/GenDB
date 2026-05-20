// Q18c: MIN(mi.info), MIN(mi_idx.info), MIN(t.title)
// over movies with mi.info IN (6 genres), mi_idx.info_type=votes,
// and at least one cast_info row with note IN (5 writer notes) and gender='m'.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <filesystem>
#include "timing_utils.h"
#include "cli_params.h"

namespace fs = std::filesystem;

struct Mapped {
    const void* ptr = nullptr;
    size_t bytes = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { std::fprintf(stderr, "stat failed: %s\n", path.c_str()); std::exit(1); }
    Mapped m;
    m.bytes = (size_t)st.st_size;
    if (m.bytes == 0) { m.ptr = nullptr; ::close(fd); return m; }
    void* p = ::mmap(nullptr, m.bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    m.ptr = p;
    ::close(fd);
    ::madvise(p, m.bytes, MADV_WILLNEED);
    return m;
}

// Compare two byte ranges lexicographically.
static inline int bcmp_lex(const char* a, int la, const char* b, int lb) {
    int mn = la < lb ? la : lb;
    int c = std::memcmp(a, b, (size_t)mn);
    if (c != 0) return c;
    if (la == lb) return 0;
    return la < lb ? -1 : 1;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // --- mmap all needed files ---
    Mapped it_off_m, it_dat_m, it_id_m;
    Mapped mi_off_m, mi_dat_m, mi_iti_m, mi_mid_m;
    Mapped mit_off_m, mit_row_m;
    Mapped mi_idx_off_m, mi_idx_dat_m, mi_idx_iti_m, mi_idx_mid_m;
    Mapped mi_idx_movieid_off_m;
    Mapped ci_off_m, ci_dat_m, ci_pid_m, ci_mid_m;
    Mapped ci_movieid_off_m;
    Mapped n_gender_m, n_gd_off_m, n_gd_dat_m;
    Mapped t_off_m, t_dat_m;

    {
        GENDB_PHASE("data_loading");
        it_off_m = map_file(store + "/info_type/info.off");
        it_dat_m = map_file(store + "/info_type/info.dat");
        it_id_m  = map_file(store + "/info_type/id.bin");

        mi_off_m = map_file(store + "/movie_info/info.off");
        mi_dat_m = map_file(store + "/movie_info/info.dat");
        mi_iti_m = map_file(store + "/movie_info/info_type_id.bin");
        mi_mid_m = map_file(store + "/movie_info/movie_id.bin");
        mit_off_m = map_file(store + "/_idx/movie_info__info_type_id__offsets.bin");
        mit_row_m = map_file(store + "/_idx/movie_info__info_type_id__rowids.bin");

        mi_idx_off_m = map_file(store + "/movie_info_idx/info.off");
        mi_idx_dat_m = map_file(store + "/movie_info_idx/info.dat");
        mi_idx_iti_m = map_file(store + "/movie_info_idx/info_type_id.bin");
        mi_idx_mid_m = map_file(store + "/movie_info_idx/movie_id.bin");
        mi_idx_movieid_off_m = map_file(store + "/_idx/movie_info_idx__movie_id__offsets.bin");

        ci_off_m = map_file(store + "/cast_info/note.off");
        ci_dat_m = map_file(store + "/cast_info/note.dat");
        ci_pid_m = map_file(store + "/cast_info/person_id.bin");
        ci_mid_m = map_file(store + "/cast_info/movie_id.bin");
        ci_movieid_off_m = map_file(store + "/_idx/cast_info__movie_id__offsets.bin");

        n_gender_m = map_file(store + "/name/gender.bin");
        n_gd_off_m = map_file(store + "/name/gender.dict.off");
        n_gd_dat_m = map_file(store + "/name/gender.dict.dat");

        t_off_m = map_file(store + "/title/title.off");
        t_dat_m = map_file(store + "/title/title.dat");
    }

    const int64_t* it_off  = (const int64_t*)it_off_m.ptr;
    const char*    it_dat  = (const char*)it_dat_m.ptr;
    const int32_t* it_id   = (const int32_t*)it_id_m.ptr;
    const int64_t  it_rows = (int64_t)(it_off_m.bytes / sizeof(int64_t)) - 1;

    const int64_t* mi_off  = (const int64_t*)mi_off_m.ptr;
    const char*    mi_dat  = (const char*)mi_dat_m.ptr;
    const int32_t* mi_iti  = (const int32_t*)mi_iti_m.ptr;
    const int32_t* mi_mid  = (const int32_t*)mi_mid_m.ptr;
    const int32_t* mit_off = (const int32_t*)mit_off_m.ptr;
    const int32_t* mit_row = (const int32_t*)mit_row_m.ptr;

    const int64_t* mi_idx_off = (const int64_t*)mi_idx_off_m.ptr;
    const char*    mi_idx_dat = (const char*)mi_idx_dat_m.ptr;
    const int32_t* mi_idx_iti = (const int32_t*)mi_idx_iti_m.ptr;
    const int32_t* mi_idx_movieid_off = (const int32_t*)mi_idx_movieid_off_m.ptr;

    const int64_t* ci_off  = (const int64_t*)ci_off_m.ptr;
    const char*    ci_dat  = (const char*)ci_dat_m.ptr;
    const int32_t* ci_pid  = (const int32_t*)ci_pid_m.ptr;
    const int32_t* ci_movieid_off = (const int32_t*)ci_movieid_off_m.ptr;
    (void)mi_mid; (void)mi_idx_mid_m; // movie_id columns used implicitly via indexes

    const int8_t*  n_gender = (const int8_t*)n_gender_m.ptr;
    const int64_t  n_rows = (int64_t)n_gender_m.bytes; // 1 byte per row
    (void)n_rows;
    const int64_t* n_gd_off = (const int64_t*)n_gd_off_m.ptr;
    const char*    n_gd_dat = (const char*)n_gd_dat_m.ptr;
    const int64_t  n_gd_rows = (int64_t)(n_gd_off_m.bytes / sizeof(int64_t)) - 1;

    const int64_t* t_off = (const int64_t*)t_off_m.ptr;
    const char*    t_dat = (const char*)t_dat_m.ptr;
    const int64_t  t_rows = (int64_t)(t_off_m.bytes / sizeof(int64_t)) - 1;

    // --- Resolve it_genres, it_votes ---
    int32_t it_genres = -1, it_votes = -1;
    {
        GENDB_PHASE("resolve_constants");
        const char* g_str = "genres"; size_t g_len = 6;
        const char* v_str = "votes";  size_t v_len = 5;
        for (int64_t i = 0; i < it_rows; ++i) {
            int64_t a = it_off[i], b = it_off[i+1];
            size_t L = (size_t)(b - a);
            if (L == g_len && std::memcmp(it_dat + a, g_str, g_len) == 0) it_genres = it_id[i];
            else if (L == v_len && std::memcmp(it_dat + a, v_str, v_len) == 0) it_votes = it_id[i];
        }
        if (it_genres < 0 || it_votes < 0) {
            std::fprintf(stderr, "info_type not found: genres=%d votes=%d\n", it_genres, it_votes);
            return 1;
        }
    }

    // --- Resolve g_m (gender dict code for 'm') ---
    int8_t g_m = -1;
    for (int64_t i = 0; i < n_gd_rows; ++i) {
        int64_t a = n_gd_off[i], b = n_gd_off[i+1];
        if ((b - a) == 1 && n_gd_dat[a] == 'm') { g_m = (int8_t)i; break; }
    }
    if (g_m < 0) { std::fprintf(stderr, "gender 'm' not found\n"); return 1; }

    // --- Genre set: 6 strings ---
    static const char* GENRES[] = {"Horror","Action","Sci-Fi","Thriller","Crime","War"};
    static const int   GENRE_LEN[] = {6,6,6,8,5,3};

    auto is_genre = [&](const char* s, int len) -> bool {
        // length-prune then equality
        for (int k = 0; k < 6; ++k) {
            if (GENRE_LEN[k] == len && std::memcmp(s, GENRES[k], (size_t)len) == 0) return true;
        }
        return false;
    };

    // --- Writer note set: 5 strings ---
    static const char* NOTES[] = {"(writer)","(head writer)","(written by)","(story)","(story editor)"};
    static const int   NOTE_LEN[] = {8,13,12,7,14};
    auto is_writer_note = [&](const char* s, int len) -> bool {
        for (int k = 0; k < 5; ++k) {
            if (NOTE_LEN[k] == len && std::memcmp(s, NOTES[k], (size_t)len) == 0) return true;
        }
        return false;
    };

    // --- Build set T of movie_ids whose mi.info is in genre set ---
    // Use a dense per-movie array: best_genre_off/len = min over genres matched (lex by string)
    // Movie ids in 1..t_rows.
    // We can use 3-byte tag (-1 sentinel via separate "has" bitset). Use simple int64 off,int32 len.
    // To save memory: store off as int64 (since file size can be > 2^31).
    int64_t movie_capacity = t_rows + 2; // include sentinel
    std::vector<int64_t> mv_genre_off(movie_capacity, -1);
    std::vector<int32_t> mv_genre_len(movie_capacity, 0);

    int32_t lo_g = mit_off[it_genres];
    int32_t hi_g = mit_off[it_genres + 1];

    {
        GENDB_PHASE("scan_movie_info_genres_filter");
        // Single-threaded: small range, varlen needs lex-min update
        for (int32_t k = lo_g; k < hi_g; ++k) {
            int32_t r = mit_row[k];
            int64_t a = mi_off[r], b = mi_off[r+1];
            int L = (int)(b - a);
            if (!is_genre(mi_dat + a, L)) continue;
            int32_t mv = mi_mid[r];
            if (mv < 1 || mv > t_rows) continue;
            if (mv_genre_off[mv] < 0) {
                mv_genre_off[mv] = a;
                mv_genre_len[mv] = L;
            } else {
                // keep lex-min
                int cmp = bcmp_lex(mi_dat + a, L,
                                   mi_dat + mv_genre_off[mv], mv_genre_len[mv]);
                if (cmp < 0) {
                    mv_genre_off[mv] = a;
                    mv_genre_len[mv] = L;
                }
            }
        }
    }

    // Collect list of qualifying movies
    std::vector<int32_t> T_movies;
    T_movies.reserve(400000);
    {
        GENDB_PHASE("build_movie_set_T");
        for (int32_t mv = 1; mv <= (int32_t)t_rows; ++mv) {
            if (mv_genre_off[mv] >= 0) T_movies.push_back(mv);
        }
    }

    // --- Probe phase: for each mv in T, look up mi_idx for votes, then ci for writer ---
    struct Best {
        // best mi.info (genre) — track via offset into mi_dat
        int64_t mi_info_off = -1; int32_t mi_info_len = 0;
        // best mi_idx.info — into mi_idx_dat
        int64_t mi_idx_info_off = -1; int32_t mi_idx_info_len = 0;
        // best t.title — into t_dat
        int64_t t_title_off = -1; int32_t t_title_len = 0;
    };

    unsigned T = std::max(1u, std::thread::hardware_concurrency());
    if (T > 12) T = 12;
    if ((int)T > (int)T_movies.size()) T = std::max((size_t)1, T_movies.size());

    std::vector<Best> per_thread(T);

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> ths;
        size_t total = T_movies.size();
        for (unsigned ti = 0; ti < T; ++ti) {
            ths.emplace_back([&, ti]() {
                size_t lo = total * ti / T;
                size_t hi = total * (ti + 1) / T;
                Best local;
                for (size_t i = lo; i < hi; ++i) {
                    int32_t mv = T_movies[i];

                    // 1) Probe mi_idx by movie_id for it_votes
                    int32_t va = mi_idx_movieid_off[mv];
                    int32_t vb = mi_idx_movieid_off[mv + 1];
                    int64_t votes_off = -1; int32_t votes_len = 0;
                    for (int32_t r = va; r < vb; ++r) {
                        if (mi_idx_iti[r] == it_votes) {
                            int64_t a = mi_idx_off[r], b = mi_idx_off[r+1];
                            votes_off = a; votes_len = (int32_t)(b - a);
                            break; // typically only one votes row
                        }
                    }
                    if (votes_off < 0) continue;

                    // 2) Probe cast_info by movie_id for writer note + gender='m'
                    int32_t ca = ci_movieid_off[mv];
                    int32_t cb = ci_movieid_off[mv + 1];
                    bool qualifies = false;
                    for (int32_t r = ca; r < cb; ++r) {
                        int64_t na = ci_off[r], nb = ci_off[r+1];
                        int L = (int)(nb - na);
                        if (L == 0) continue;
                        if (!is_writer_note(ci_dat + na, L)) continue;
                        int32_t pid = ci_pid[r];
                        if (pid < 1) continue;
                        if (n_gender[pid - 1] != g_m) continue;
                        qualifies = true;
                        break;
                    }
                    if (!qualifies) continue;

                    // 3) Update MINs.
                    // mi.info: take min over all genre rows for this movie (we stored per-movie min already)
                    int64_t g_off = mv_genre_off[mv];
                    int32_t g_len = mv_genre_len[mv];
                    if (local.mi_info_off < 0 ||
                        bcmp_lex(mi_dat + g_off, g_len,
                                 mi_dat + local.mi_info_off, local.mi_info_len) < 0) {
                        local.mi_info_off = g_off;
                        local.mi_info_len = g_len;
                    }
                    // mi_idx.info
                    if (local.mi_idx_info_off < 0 ||
                        bcmp_lex(mi_idx_dat + votes_off, votes_len,
                                 mi_idx_dat + local.mi_idx_info_off, local.mi_idx_info_len) < 0) {
                        local.mi_idx_info_off = votes_off;
                        local.mi_idx_info_len = votes_len;
                    }
                    // t.title (movie_id is 1-based -> title row index = mv-1)
                    int64_t ta = t_off[mv - 1], tb = t_off[mv];
                    int32_t tl = (int32_t)(tb - ta);
                    if (local.t_title_off < 0 ||
                        bcmp_lex(t_dat + ta, tl,
                                 t_dat + local.t_title_off, local.t_title_len) < 0) {
                        local.t_title_off = ta;
                        local.t_title_len = tl;
                    }
                }
                per_thread[ti] = local;
            });
        }
        for (auto& th : ths) th.join();
    }

    // Reduce
    Best global;
    for (auto& b : per_thread) {
        if (b.mi_info_off >= 0) {
            if (global.mi_info_off < 0 ||
                bcmp_lex(mi_dat + b.mi_info_off, b.mi_info_len,
                         mi_dat + global.mi_info_off, global.mi_info_len) < 0) {
                global.mi_info_off = b.mi_info_off;
                global.mi_info_len = b.mi_info_len;
            }
        }
        if (b.mi_idx_info_off >= 0) {
            if (global.mi_idx_info_off < 0 ||
                bcmp_lex(mi_idx_dat + b.mi_idx_info_off, b.mi_idx_info_len,
                         mi_idx_dat + global.mi_idx_info_off, global.mi_idx_info_len) < 0) {
                global.mi_idx_info_off = b.mi_idx_info_off;
                global.mi_idx_info_len = b.mi_idx_info_len;
            }
        }
        if (b.t_title_off >= 0) {
            if (global.t_title_off < 0 ||
                bcmp_lex(t_dat + b.t_title_off, b.t_title_len,
                         t_dat + global.t_title_off, global.t_title_len) < 0) {
                global.t_title_off = b.t_title_off;
                global.t_title_len = b.t_title_len;
            }
        }
    }

    // --- Output ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q18c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_budget,movie_votes,movie_title\n");
        auto emit = [&](const char* d, int64_t off, int32_t len) {
            if (off < 0) return; // nothing
            // CSV: quote if comma, quote, or newline
            bool need_quote = false;
            for (int32_t i = 0; i < len; ++i) {
                char c = d[off + i];
                if (c == ',' || c == '"' || c == '\n') { need_quote = true; break; }
            }
            if (!need_quote) {
                std::fwrite(d + off, 1, (size_t)len, f);
            } else {
                std::fputc('"', f);
                for (int32_t i = 0; i < len; ++i) {
                    char c = d[off + i];
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            }
        };
        emit(mi_dat, global.mi_info_off, global.mi_info_len);
        std::fputc(',', f);
        emit(mi_idx_dat, global.mi_idx_info_off, global.mi_idx_info_len);
        std::fputc(',', f);
        emit(t_dat, global.t_title_off, global.t_title_len);
        std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
