// Q32a - movie pairs joined via keyword '10,000-mile-club'
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "timing_utils.h"

namespace fs = std::filesystem;

struct Mapped {
    const void* data = nullptr;
    size_t size = 0;
    int fd = -1;
};

static Mapped mmap_file(const std::string& path) {
    Mapped m;
    m.fd = ::open(path.c_str(), O_RDONLY);
    if (m.fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", path.c_str());
        std::exit(1);
    }
    struct stat st;
    if (::fstat(m.fd, &st) != 0) {
        std::fprintf(stderr, "fstat failed: %s\n", path.c_str());
        std::exit(1);
    }
    m.size = (size_t)st.st_size;
    if (m.size == 0) {
        m.data = nullptr;
        return m;
    }
    m.data = ::mmap(nullptr, m.size, PROT_READ, MAP_SHARED, m.fd, 0);
    if (m.data == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed: %s\n", path.c_str());
        std::exit(1);
    }
    return m;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];

    // mmaps
    Mapped kw_off, kw_dat;
    Mapped mk_keyword_id, mk_movie_id;
    Mapped mkk_off, mkk_row;
    Mapped ml_movie_id_idx_off;
    Mapped ml_linked_movie_id, ml_link_type_id;
    Mapped lt_link_off, lt_link_dat;
    Mapped title_off, title_dat;

    int32_t k_id = -1;
    std::vector<int32_t> t1_ids;

    {
        GENDB_PHASE("data_loading");
        kw_off = mmap_file(store + "/keyword/keyword.off");
        kw_dat = mmap_file(store + "/keyword/keyword.dat");
        mk_keyword_id = mmap_file(store + "/movie_keyword/keyword_id.bin");
        mk_movie_id   = mmap_file(store + "/movie_keyword/movie_id.bin");
        mkk_off = mmap_file(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mkk_row = mmap_file(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
        ml_movie_id_idx_off = mmap_file(store + "/_idx/movie_link__movie_id__offsets.bin");
        ml_linked_movie_id = mmap_file(store + "/movie_link/linked_movie_id.bin");
        ml_link_type_id    = mmap_file(store + "/movie_link/link_type_id.bin");
        lt_link_off = mmap_file(store + "/link_type/link.off");
        lt_link_dat = mmap_file(store + "/link_type/link.dat");
        title_off = mmap_file(store + "/title/title.off");
        title_dat = mmap_file(store + "/title/title.dat");
    }

    const int64_t* kw_off_arr = static_cast<const int64_t*>(kw_off.data);
    const char* kw_dat_arr = static_cast<const char*>(kw_dat.data);
    size_t kw_off_n = kw_off.size / sizeof(int64_t);  // rows + 1 entries

    {
        GENDB_PHASE("resolve_keyword");
        const char target[] = "10,000-mile-club";
        const size_t target_len = sizeof(target) - 1; // 16
        size_t n_rows = kw_off_n - 1;
        for (size_t i = 0; i < n_rows; ++i) {
            int64_t a = kw_off_arr[i];
            int64_t b = kw_off_arr[i+1];
            if ((size_t)(b - a) != target_len) continue;
            if (std::memcmp(kw_dat_arr + a, target, target_len) == 0) {
                k_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    if (k_id < 0) {
        // No keyword found -> empty result
        fs::create_directories(results);
        FILE* f = std::fopen((results + "/Q32a.csv").c_str(), "w");
        std::fprintf(f, "link_type,first_movie,second_movie\n,,\n");
        std::fclose(f);
        return 0;
    }

    const int32_t* mk_movie_id_arr = static_cast<const int32_t*>(mk_movie_id.data);
    const int32_t* mkk_off_arr = static_cast<const int32_t*>(mkk_off.data);
    const int32_t* mkk_row_arr = static_cast<const int32_t*>(mkk_row.data);

    {
        GENDB_PHASE("csr_lookup_movie_keyword");
        int32_t lo = mkk_off_arr[k_id];
        int32_t hi = mkk_off_arr[k_id + 1];
        t1_ids.reserve(hi - lo);
        for (int32_t k = lo; k < hi; ++k) {
            int32_t r = mkk_row_arr[k];
            t1_ids.push_back(mk_movie_id_arr[r]);
        }
    }

    const int32_t* mlm_off_arr = static_cast<const int32_t*>(ml_movie_id_idx_off.data);
    const int32_t* ml_linked_arr = static_cast<const int32_t*>(ml_linked_movie_id.data);
    const int32_t* ml_lt_arr = static_cast<const int32_t*>(ml_link_type_id.data);
    const int64_t* lt_off_arr = static_cast<const int64_t*>(lt_link_off.data);
    const char* lt_dat_arr = static_cast<const char*>(lt_link_dat.data);
    const int64_t* title_off_arr = static_cast<const int64_t*>(title_off.data);
    const char* title_dat_arr = static_cast<const char*>(title_dat.data);

    // Running minima as string_view (lexicographic min)
    bool have_min = false;
    std::string_view min_lt;
    std::string_view min_t1;
    std::string_view min_t2;

    auto sv_at = [](const int64_t* off, const char* dat, int32_t row_idx) -> std::string_view {
        int64_t a = off[row_idx];
        int64_t b = off[row_idx + 1];
        return std::string_view(dat + a, (size_t)(b - a));
    };

    {
        GENDB_PHASE("main_scan");
        for (int32_t t1_id : t1_ids) {
            int32_t lo = mlm_off_arr[t1_id];     // ml table sorted by movie_id, offsets indexed by movie_id (1-based to size+1)
            int32_t hi = mlm_off_arr[t1_id + 1];
            if (lo >= hi) continue;
            std::string_view t1_title = sv_at(title_off_arr, title_dat_arr, t1_id - 1);
            for (int32_t r = lo; r < hi; ++r) {
                int32_t t2_id = ml_linked_arr[r];
                int32_t lt_id = ml_lt_arr[r];
                std::string_view t2_title = sv_at(title_off_arr, title_dat_arr, t2_id - 1);
                std::string_view lt_link = sv_at(lt_off_arr, lt_dat_arr, lt_id - 1);

                if (!have_min) {
                    min_lt = lt_link;
                    min_t1 = t1_title;
                    min_t2 = t2_title;
                    have_min = true;
                } else {
                    if (lt_link < min_lt) min_lt = lt_link;
                    if (t1_title < min_t1) min_t1 = t1_title;
                    if (t2_title < min_t2) min_t2 = t2_title;
                }
            }
        }
    }

    {
        GENDB_PHASE("output");
        fs::create_directories(results);
        FILE* f = std::fopen((results + "/Q32a.csv").c_str(), "w");
        std::fprintf(f, "link_type,first_movie,second_movie\n");
        if (!have_min) {
            std::fprintf(f, ",,\n");
        } else {
            // CSV escape: wrap in quotes if contains comma/quote/newline
            auto write_field = [&](std::string_view s) {
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
            write_field(min_lt);
            std::fputc(',', f);
            write_field(min_t1);
            std::fputc(',', f);
            write_field(min_t2);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
