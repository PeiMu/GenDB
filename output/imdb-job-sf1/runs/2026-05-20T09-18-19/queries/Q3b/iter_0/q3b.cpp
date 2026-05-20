// Q3b: SELECT MIN(t.title) FROM keyword, movie_info, movie_keyword, title
//      WHERE k.keyword LIKE '%sequel%' AND mi.info = 'Bulgaria'
//        AND t.production_year > 2010 AND star-join on title.id.
//
// Pipeline:
//   1) Scan keyword.keyword varlen via memmem('sequel') -> seq_ids
//   2) Walk movie_keyword__keyword_id CSR to gather candidate movie_ids
//   3) Filter candidates by title.production_year > 2010 (dense identity)
//   4) For each surviving mv, probe movie_info__movie_id offsets range,
//      length prefilter (==8) then memcmp 'Bulgaria'; existence semantics
//   5) Track running MIN(t.title) over hits

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_set>
#include <sys/types.h>

#include "timing_utils.h"
#include "cli_params.h"

struct Mapped {
    const void* data = nullptr;
    size_t size = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st{};
    if (::fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    Mapped m;
    m.size = (size_t)st.st_size;
    if (m.size == 0) { ::close(fd); return m; }
    void* p = ::mmap(nullptr, m.size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    ::close(fd);
    m.data = p;
    return m;
}

static void ensure_dir(const std::string& dir) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) return;
    std::string cmd = "mkdir -p " + dir;
    (void)std::system(cmd.c_str());
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    ensure_dir(results_dir);

    GENDB_PHASE("total");

    // ----- Load files -----
    Mapped m_kw_off, m_kw_dat;
    Mapped m_mk_kid_off, m_mk_kid_rowids, m_mk_movie_id;
    Mapped m_title_year, m_title_off, m_title_dat;
    Mapped m_mi_off_idx, m_mi_info_off, m_mi_info_dat;
    {
        GENDB_PHASE("data_loading");
        m_kw_off        = map_file(gendb_dir + "/keyword/keyword.off");
        m_kw_dat        = map_file(gendb_dir + "/keyword/keyword.dat");
        m_mk_kid_off    = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__offsets.bin");
        m_mk_kid_rowids = map_file(gendb_dir + "/_idx/movie_keyword__keyword_id__rowids.bin");
        m_mk_movie_id   = map_file(gendb_dir + "/movie_keyword/movie_id.bin");
        m_title_year    = map_file(gendb_dir + "/title/production_year.bin");
        m_title_off     = map_file(gendb_dir + "/title/title.off");
        m_title_dat     = map_file(gendb_dir + "/title/title.dat");
        m_mi_off_idx    = map_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        m_mi_info_off   = map_file(gendb_dir + "/movie_info/info.off");
        m_mi_info_dat   = map_file(gendb_dir + "/movie_info/info.dat");
    }

    const uint64_t* kw_off = static_cast<const uint64_t*>(m_kw_off.data);
    const char*     kw_dat = static_cast<const char*>(m_kw_dat.data);
    const int32_t kw_rows = (int32_t)((m_kw_off.size / 8) - 1);

    const int32_t* mk_kid_off    = static_cast<const int32_t*>(m_mk_kid_off.data);
    const int32_t* mk_kid_rowids = static_cast<const int32_t*>(m_mk_kid_rowids.data);
    const int32_t* mk_movie_id   = static_cast<const int32_t*>(m_mk_movie_id.data);
    const int32_t mk_kid_off_n   = (int32_t)(m_mk_kid_off.size / 4);  // n_keywords + 1

    const int32_t* title_year = static_cast<const int32_t*>(m_title_year.data);
    const int32_t title_rows  = (int32_t)(m_title_year.size / 4);
    const uint64_t* title_off = static_cast<const uint64_t*>(m_title_off.data);
    const char* title_dat     = static_cast<const char*>(m_title_dat.data);

    const int32_t* mi_off_idx = static_cast<const int32_t*>(m_mi_off_idx.data);
    const int32_t mi_off_idx_n = (int32_t)(m_mi_off_idx.size / 4);
    const uint64_t* mi_info_off = static_cast<const uint64_t*>(m_mi_info_off.data);
    const char* mi_info_dat = static_cast<const char*>(m_mi_info_dat.data);

    // ----- Step 1: scan keyword.keyword for '%sequel%' -----
    std::vector<int32_t> seq_ids;
    seq_ids.reserve(256);
    {
        GENDB_PHASE("keyword_scan");
        const char* needle = "sequel";
        const size_t needle_len = 6;
        // keyword.id is 1-indexed dense identity (row i -> id i+1).
        for (int32_t i = 0; i < kw_rows; ++i) {
            uint64_t lo = kw_off[i];
            uint64_t hi = kw_off[i + 1];
            size_t len = (size_t)(hi - lo);
            if (len < needle_len) continue;
            if (::memmem(kw_dat + lo, len, needle, needle_len)) {
                seq_ids.push_back(i + 1);
            }
        }
    }

    // ----- Step 2: CSR walk movie_keyword by keyword_id -> candidate movies -----
    // Use a hash set for dedup; estimated ~50k.
    std::unordered_set<int32_t> candidate_set;
    candidate_set.reserve(131072);
    {
        GENDB_PHASE("csr_lookup_movie_keyword");
        for (int32_t kid : seq_ids) {
            if (kid + 1 >= mk_kid_off_n) continue;
            int32_t lo = mk_kid_off[kid];
            int32_t hi = mk_kid_off[kid + 1];
            for (int32_t p = lo; p < hi; ++p) {
                int32_t mk_row = mk_kid_rowids[p];
                int32_t mv = mk_movie_id[mk_row];
                candidate_set.insert(mv);
            }
        }
    }

    // ----- Step 3: filter by title.production_year > 2010 -----
    std::vector<int32_t> surviving;
    surviving.reserve(candidate_set.size());
    {
        GENDB_PHASE("filter_title_year");
        // title.id is 1-indexed dense identity: id mv -> row mv-1.
        for (int32_t mv : candidate_set) {
            if (mv < 1 || mv > title_rows) continue;
            int32_t y = title_year[mv - 1];
            if (y != INT32_MIN && y > 2010) {
                surviving.push_back(mv);
            }
        }
    }

    // ----- Step 4: probe movie_info for 'Bulgaria' (existence) + track MIN(title) -----
    std::string min_title;
    bool have_min = false;
    {
        GENDB_PHASE("main_scan");
        const char* target = "Bulgaria";
        const size_t target_len = 8;
        for (int32_t mv : surviving) {
            if (mv + 1 >= mi_off_idx_n) continue;
            int32_t lo = mi_off_idx[mv];
            int32_t hi = mi_off_idx[mv + 1];
            bool hit = false;
            for (int32_t r = lo; r < hi; ++r) {
                uint64_t a = mi_info_off[r];
                uint64_t b = mi_info_off[r + 1];
                if ((b - a) != target_len) continue;
                if (std::memcmp(mi_info_dat + a, target, target_len) == 0) {
                    hit = true;
                    break;
                }
            }
            if (!hit) continue;
            // Fetch t.title[mv] — title.id is 1-indexed (mv -> row mv-1).
            uint64_t ta = title_off[mv - 1];
            uint64_t tb = title_off[mv];
            std::string_view tv(title_dat + ta, (size_t)(tb - ta));
            if (!have_min || tv < std::string_view(min_title)) {
                min_title.assign(tv.data(), tv.size());
                have_min = true;
            }
        }
    }

    // ----- Step 5: output CSV -----
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q3b.csv";
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "open output failed: %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_title\n");
        if (have_min) {
            std::fwrite(min_title.data(), 1, min_title.size(), f);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
