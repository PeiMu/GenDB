// Q18a — MIN(mi.info), MIN(mi_idx.info), MIN(t.title)
// over name(gender=m, name LIKE %Tim%) ⋈ cast_info(note IN (...))
//      ⋈ movie_info(info_type='budget') ⋈ movie_info_idx(info_type='votes') ⋈ title.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>
#include "timing_utils.h"
#include "cli_params.h"

using gendb::PhaseTimer;

struct Mmap {
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;
};

static Mmap map_file(const std::string& path) {
    Mmap m;
    m.fd = open(path.c_str(), O_RDONLY);
    if (m.fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(m.fd, &st) != 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    m.size = st.st_size;
    if (m.size > 0) {
        m.data = (const char*)mmap(nullptr, m.size, PROT_READ, MAP_SHARED, m.fd, 0);
        if (m.data == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    }
    return m;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string out_dir = argv[2];

    GENDB_PHASE("total");

    int32_t it_budget = 0, it_votes = 0;
    int8_t   g_m = 0;

    // Persistent mmaps used in main scan
    Mmap n_gender_m, n_name_off_m, n_name_dat_m;
    Mmap cipid_off_m, cipid_row_m;
    Mmap ci_note_off_m, ci_note_dat_m, ci_movie_id_m;
    Mmap mim_off_m, mi_itid_m, mi_info_off_m, mi_info_dat_m;
    Mmap mixm_off_m, mix_itid_m, mix_info_off_m, mix_info_dat_m;
    Mmap t_title_off_m, t_title_dat_m;

    int32_t n_rows = 0;

    {
        GENDB_PHASE("data_loading");

        // info_type dictionary scan
        Mmap it_off_m = map_file(store + "/info_type/info.off");
        Mmap it_dat_m = map_file(store + "/info_type/info.dat");
        const int64_t* it_off = (const int64_t*)it_off_m.data;
        size_t it_n = it_off_m.size / sizeof(int64_t);
        for (size_t i = 0; i + 1 < it_n; ++i) {
            std::string_view s(it_dat_m.data + it_off[i], it_off[i + 1] - it_off[i]);
            if (s == "budget") it_budget = (int32_t)(i + 1);
            else if (s == "votes") it_votes = (int32_t)(i + 1);
        }

        // name gender dict
        Mmap g_off_m = map_file(store + "/name/gender.dict.off");
        Mmap g_dat_m = map_file(store + "/name/gender.dict.dat");
        const int64_t* g_off = (const int64_t*)g_off_m.data;
        size_t g_n = g_off_m.size / sizeof(int64_t);
        for (size_t i = 0; i + 1 < g_n; ++i) {
            std::string_view s(g_dat_m.data + g_off[i], g_off[i + 1] - g_off[i]);
            if (s == "m") { g_m = (int8_t)(i + 1); break; }
        }

        n_gender_m   = map_file(store + "/name/gender.bin");
        n_rows       = (int32_t)n_gender_m.size; // int8 column
        n_name_off_m = map_file(store + "/name/name.off");
        n_name_dat_m = map_file(store + "/name/name.dat");

        cipid_off_m   = map_file(store + "/_idx/cast_info__person_id__offsets.bin");
        cipid_row_m   = map_file(store + "/_idx/cast_info__person_id__rowids.bin");
        ci_note_off_m = map_file(store + "/cast_info/note.off");
        ci_note_dat_m = map_file(store + "/cast_info/note.dat");
        ci_movie_id_m = map_file(store + "/cast_info/movie_id.bin");

        mim_off_m     = map_file(store + "/_idx/movie_info__movie_id__offsets.bin");
        mi_itid_m     = map_file(store + "/movie_info/info_type_id.bin");
        mi_info_off_m = map_file(store + "/movie_info/info.off");
        mi_info_dat_m = map_file(store + "/movie_info/info.dat");

        mixm_off_m     = map_file(store + "/_idx/movie_info_idx__movie_id__offsets.bin");
        mix_itid_m     = map_file(store + "/movie_info_idx/info_type_id.bin");
        mix_info_off_m = map_file(store + "/movie_info_idx/info.off");
        mix_info_dat_m = map_file(store + "/movie_info_idx/info.dat");

        t_title_off_m = map_file(store + "/title/title.off");
        t_title_dat_m = map_file(store + "/title/title.dat");
    }

    if (it_budget == 0 || it_votes == 0 || g_m == 0) {
        std::fprintf(stderr, "dim resolution failed: it_budget=%d it_votes=%d g_m=%d\n",
                     it_budget, it_votes, (int)g_m);
        return 1;
    }

    const int8_t*  n_gender      = (const int8_t*)n_gender_m.data;
    const int64_t* n_name_off    = (const int64_t*)n_name_off_m.data;
    const char*    n_name_dat    = n_name_dat_m.data;
    const int32_t* cipid_off     = (const int32_t*)cipid_off_m.data;
    const int32_t* cipid_row     = (const int32_t*)cipid_row_m.data;
    const int64_t* ci_note_off   = (const int64_t*)ci_note_off_m.data;
    const char*    ci_note_dat   = ci_note_dat_m.data;
    const int32_t* ci_movie_id   = (const int32_t*)ci_movie_id_m.data;
    const int32_t* mim_off       = (const int32_t*)mim_off_m.data;
    const int32_t* mi_itid       = (const int32_t*)mi_itid_m.data;
    const int64_t* mi_info_off   = (const int64_t*)mi_info_off_m.data;
    const char*    mi_info_dat   = mi_info_dat_m.data;
    const int32_t* mixm_off      = (const int32_t*)mixm_off_m.data;
    const int32_t* mix_itid      = (const int32_t*)mix_itid_m.data;
    const int64_t* mix_info_off  = (const int64_t*)mix_info_off_m.data;
    const char*    mix_info_dat  = mix_info_dat_m.data;
    const int64_t* t_title_off   = (const int64_t*)t_title_off_m.data;
    const char*    t_title_dat   = t_title_dat_m.data;

    // ---- Phase: filter name by gender='m' AND name LIKE '%Tim%' ----
    std::vector<int32_t> pid_set;
    {
        GENDB_PHASE("filter_name");
        const char NEEDLE[] = "Tim";
        const size_t NLEN = 3;
        int nthreads = omp_get_max_threads();
        std::vector<std::vector<int32_t>> local(nthreads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& lv = local[tid];
            #pragma omp for schedule(static)
            for (int32_t i = 0; i < n_rows; ++i) {
                if (n_gender[i] != g_m) continue;
                int64_t lo = n_name_off[i];
                int64_t hi = n_name_off[i + 1];
                size_t len = (size_t)(hi - lo);
                if (len < NLEN) continue;
                if (memmem(n_name_dat + lo, len, NEEDLE, NLEN)) {
                    lv.push_back(i + 1); // dense PK: name id = row + 1
                }
            }
        }
        size_t total = 0;
        for (auto& lv : local) total += lv.size();
        pid_set.reserve(total);
        for (auto& lv : local) pid_set.insert(pid_set.end(), lv.begin(), lv.end());
    }

    // ---- Phase: walk CSR per pid, filter note, range-probe mi + mi_idx, aggregate MIN ----
    std::string min_budget, min_votes, min_title;
    bool any_qualified = false;

    {
        GENDB_PHASE("main_scan");
        const char NOTE1[] = "(producer)";
        const size_t NL1 = 10;
        const char NOTE2[] = "(executive producer)";
        const size_t NL2 = 20;

        int nthreads = omp_get_max_threads();
        std::vector<std::string> lmin_b(nthreads), lmin_v(nthreads), lmin_t(nthreads);
        std::vector<char> lhas(nthreads, 0);

        const size_t P = pid_set.size();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string mb, mv, mt;
            bool has = false;

            #pragma omp for schedule(dynamic, 64)
            for (size_t pi = 0; pi < P; ++pi) {
                int32_t pid = pid_set[pi];
                int32_t lo = cipid_off[pid];
                int32_t hi = cipid_off[pid + 1];

                for (int32_t k = lo; k < hi; ++k) {
                    int32_t r = cipid_row[k];

                    // note IN ('(producer)','(executive producer)')
                    int64_t nlo = ci_note_off[r];
                    int64_t nhi = ci_note_off[r + 1];
                    size_t nlen = (size_t)(nhi - nlo);
                    if (nlen != NL1 && nlen != NL2) continue;
                    const char* np = ci_note_dat + nlo;
                    bool note_ok = false;
                    if (nlen == NL1) {
                        note_ok = (std::memcmp(np, NOTE1, NL1) == 0);
                    } else { // NL2
                        note_ok = (std::memcmp(np, NOTE2, NL2) == 0);
                    }
                    if (!note_ok) continue;

                    int32_t movie = ci_movie_id[r];
                    if (movie <= 0) continue;

                    // movie_info range probe for info_type_id == it_budget
                    int32_t mlo = mim_off[movie];
                    int32_t mhi = mim_off[movie + 1];
                    std::string_view best_b;
                    bool found_b = false;
                    for (int32_t rr = mlo; rr < mhi; ++rr) {
                        if (mi_itid[rr] != it_budget) continue;
                        int64_t io = mi_info_off[rr];
                        int64_t ie = mi_info_off[rr + 1];
                        std::string_view s(mi_info_dat + io, (size_t)(ie - io));
                        if (!found_b || s < best_b) { best_b = s; found_b = true; }
                    }
                    if (!found_b) continue;

                    // movie_info_idx range probe for info_type_id == it_votes
                    int32_t xlo = mixm_off[movie];
                    int32_t xhi = mixm_off[movie + 1];
                    std::string_view best_v;
                    bool found_v = false;
                    for (int32_t rr = xlo; rr < xhi; ++rr) {
                        if (mix_itid[rr] != it_votes) continue;
                        int64_t io = mix_info_off[rr];
                        int64_t ie = mix_info_off[rr + 1];
                        std::string_view s(mix_info_dat + io, (size_t)(ie - io));
                        if (!found_v || s < best_v) { best_v = s; found_v = true; }
                    }
                    if (!found_v) continue;

                    // title for this movie (dense PK: row = id - 1)
                    int64_t to = t_title_off[movie - 1];
                    int64_t te = t_title_off[movie];
                    std::string_view title_sv(t_title_dat + to, (size_t)(te - to));

                    // Update local MINs (independent per column).
                    if (!has || best_b < std::string_view(mb)) mb.assign(best_b);
                    if (!has || best_v < std::string_view(mv)) mv.assign(best_v);
                    if (!has || title_sv < std::string_view(mt)) mt.assign(title_sv);
                    has = true;
                }
            }

            lmin_b[tid] = std::move(mb);
            lmin_v[tid] = std::move(mv);
            lmin_t[tid] = std::move(mt);
            lhas[tid] = has ? 1 : 0;
        }

        for (int t = 0; t < nthreads; ++t) {
            if (!lhas[t]) continue;
            if (!any_qualified || lmin_b[t] < min_budget) min_budget = lmin_b[t];
            if (!any_qualified || lmin_v[t] < min_votes)  min_votes  = lmin_v[t];
            if (!any_qualified || lmin_t[t] < min_title)  min_title  = lmin_t[t];
            any_qualified = true;
        }
    }

    // ---- Output CSV ----
    {
        GENDB_PHASE("output");
        std::string mkdir = "mkdir -p '" + out_dir + "'";
        (void)system(mkdir.c_str());

        std::string path = out_dir + "/Q18a.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "fopen failed: %s\n", path.c_str()); return 1; }

        std::fprintf(f, "movie_budget,movie_votes,movie_title\n");

        auto needs_quote = [](const std::string& s) -> bool {
            for (char c : s) {
                if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
            }
            return false;
        };
        auto write_field = [&](const std::string& s) {
            if (needs_quote(s)) {
                std::fputc('"', f);
                for (char c : s) {
                    if (c == '"') std::fputc('"', f);
                    std::fputc(c, f);
                }
                std::fputc('"', f);
            } else {
                std::fwrite(s.data(), 1, s.size(), f);
            }
        };
        if (any_qualified) {
            write_field(min_budget); std::fputc(',', f);
            write_field(min_votes);  std::fputc(',', f);
            write_field(min_title);  std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
