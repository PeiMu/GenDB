// build_indexes.cpp
//
// Phase 2 of ingestion. Reads the columnar files produced by ingest.cpp and:
//   1. For each dimension PK table: builds a dense `id -> row_position` lookup array.
//        indexes/<table>__<id>__pos.bin   int32[max_id+2]  (-1 for missing ids)
//   2. For each fact table: sorts all column files in place by the primary FK (counting sort),
//      then builds:
//        indexes/<table>__<fk>__offsets.bin    uint64[max_id+2]  (primary CSR)
//      and any additional aux CSR indexes:
//        indexes/<table>__<aux>__offsets.bin   uint64[max_id+2]
//        indexes/<table>__<aux>__rowids.bin    int32[N]
//
// All table jobs run in parallel using a work queue.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ---------------- low-level IO -----------------------------------------------
struct MMap {
    const char* data = nullptr;
    size_t size = 0;
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr,"open %s: %s\n", path.c_str(), strerror(errno)); return false; }
        struct stat st; fstat(fd, &st);
        size = (size_t)st.st_size;
        if (size == 0) { data = nullptr; return true; }
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { fprintf(stderr,"mmap %s\n", path.c_str()); return false; }
        data = (const char*)p;
        madvise(const_cast<void*>(static_cast<const void*>(data)), size, MADV_WILLNEED);
        return true;
    }
    ~MMap() { if (data && size) munmap((void*)data, size); if (fd>=0) ::close(fd); }
};

static uint64_t read_u64(const std::string& path) {
    FILE* f = std::fopen(path.c_str(),"rb"); if (!f) { fprintf(stderr,"missing %s\n", path.c_str()); std::exit(1); }
    uint64_t v = 0; (void)std::fread(&v, sizeof(v), 1, f); std::fclose(f);
    return v;
}
static void write_bin(const std::string& path, const void* data, size_t bytes) {
    FILE* f = std::fopen(path.c_str(),"wb");
    if (!f) { fprintf(stderr,"write %s: %s\n", path.c_str(), strerror(errno)); std::exit(1); }
    if (bytes && std::fwrite(data, 1, bytes, f) != bytes) { fprintf(stderr,"write fail %s\n", path.c_str()); std::exit(1); }
    std::fclose(f);
}

// Read entire int32 column file into vector.
static std::vector<int32_t> read_i32_col(const std::string& path, uint64_t N) {
    MMap mm;
    if (!mm.open(path)) { fprintf(stderr,"cannot open %s\n", path.c_str()); std::exit(1); }
    if (mm.size != N * sizeof(int32_t)) {
        fprintf(stderr,"size mismatch on %s: %zu vs %llu*4\n", path.c_str(), mm.size, (unsigned long long)N); std::exit(1);
    }
    std::vector<int32_t> v(N);
    if (N) std::memcpy(v.data(), mm.data, N*sizeof(int32_t));
    return v;
}

// ---------------- schema declaration -----------------------------------------
struct AuxSpec { std::string col; int max_id; };

struct TblJob {
    std::string name;
    // dimension PK index
    std::string pk_col;          // empty if not a dim
    // fact sorting
    std::string sort_col;        // empty if no sort
    // columns to permute (only for fact tables that sort)
    std::vector<std::string> int_cols;        // fixed int32 (and int32-nullable use same file format)
    std::vector<std::string> char1_cols;      // fixed 1-byte
    std::vector<std::string> varlen_cols;
    // aux CSR indexes to build (after sort if sort was done)
    std::vector<AuxSpec> aux;
};

static std::vector<TblJob> all_jobs() {
    std::vector<TblJob> ts;

    // ---- dimension PK tables: only build pk_pos index, no sorting. ----
    auto dim = [&](const std::string& name) {
        TblJob j; j.name = name; j.pk_col = "id"; ts.push_back(std::move(j));
    };
    dim("title");
    dim("name");
    dim("char_name");
    dim("company_name");
    dim("keyword");
    dim("info_type");
    dim("link_type");
    dim("role_type");
    dim("kind_type");
    dim("company_type");
    dim("comp_cast_type");

    // ---- fact tables: sort by primary FK, build primary CSR (+aux CSRs). ----
    auto fact = [&](const std::string& name, const std::string& sort_col,
                    std::vector<std::string> ints, std::vector<std::string> chars,
                    std::vector<std::string> vars, std::vector<AuxSpec> aux) {
        TblJob j;
        j.name = name; j.sort_col = sort_col;
        j.int_cols = std::move(ints);
        j.char1_cols = std::move(chars);
        j.varlen_cols = std::move(vars);
        j.aux = std::move(aux);
        ts.push_back(std::move(j));
    };

    // Note: max_id passed to aux CSR is a conservative upper bound; build_aux scans for the actual max.
    fact("aka_name", "person_id",
         {"person_id"}, {}, {"name"}, {});

    fact("aka_title", "movie_id",
         {"movie_id"}, {}, {"title"}, {});

    fact("person_info", "person_id",
         {"person_id","info_type_id"}, {}, {"info","note"}, {});

    fact("complete_cast", "movie_id",
         {"movie_id","subject_id","status_id"}, {}, {}, {});

    fact("movie_link", "movie_id",
         {"movie_id","linked_movie_id","link_type_id"}, {}, {},
         {{"linked_movie_id", 0}, {"link_type_id", 0}});

    fact("movie_companies", "movie_id",
         {"movie_id","company_id","company_type_id"}, {}, {"note"},
         {{"company_id", 0}});

    fact("movie_keyword", "movie_id",
         {"movie_id","keyword_id"}, {}, {},
         {{"keyword_id", 0}});

    fact("movie_info_idx", "movie_id",
         {"movie_id","info_type_id"}, {}, {"info"},
         {{"info_type_id", 0}});

    fact("movie_info", "movie_id",
         {"movie_id","info_type_id"}, {}, {"info","note"},
         {{"info_type_id", 0}});

    fact("cast_info", "movie_id",
         {"movie_id","person_id","person_role_id","role_id"}, {}, {"note"},
         {{"person_id", 0}, {"role_id", 0}});

    return ts;
}

// ---------------- dimension PK index ----------------------------------------
static void build_pk_pos(const std::string& store, const std::string& table, const std::string& pk_col) {
    auto t0 = std::chrono::steady_clock::now();
    std::string dir = store + "/" + table;
    uint64_t N = read_u64(dir + "/__row_count.bin");
    auto ids = read_i32_col(dir + "/" + pk_col + ".bin", N);
    int32_t max_id = 0;
    for (auto v : ids) if (v > max_id) max_id = v;
    std::vector<int32_t> pos((size_t)max_id + 2, -1);
    for (uint64_t r = 0; r < N; ++r) {
        int32_t id = ids[r];
        if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
    }
    fs::create_directories(store + "/indexes");
    std::string out = store + "/indexes/" + table + "__" + pk_col + "__pos.bin";
    write_bin(out, pos.data(), pos.size() * sizeof(int32_t));
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[pk_pos %s.%s] N=%llu max_id=%d  %.2fs\n",
            table.c_str(), pk_col.c_str(), (unsigned long long)N, max_id, dt);
}

// ---------------- fact table sort + indexes ---------------------------------
// Counting sort on key column. Produces:
//   perm[0..N) — row positions in sorted order (stable)
//   offsets[0..max_key+2) — CSR offsets so rows for key k are perm[offsets[k]..offsets[k+1])
// Negative keys are bucketed at key=0.
static void counting_sort(const int32_t* key, uint64_t N,
                          std::vector<uint32_t>& perm,
                          std::vector<uint64_t>& offsets) {
    int32_t max_k = 0;
    for (uint64_t r = 0; r < N; ++r) if (key[r] > max_k) max_k = key[r];
    offsets.assign((size_t)max_k + 2, 0);
    for (uint64_t r = 0; r < N; ++r) {
        int32_t k = key[r] < 0 ? 0 : key[r];
        offsets[(size_t)k + 1]++;
    }
    for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
    perm.assign(N, 0);
    std::vector<uint64_t> cur = offsets;
    for (uint64_t r = 0; r < N; ++r) {
        int32_t k = key[r] < 0 ? 0 : key[r];
        perm[cur[(size_t)k]++] = (uint32_t)r;
    }
}

// Apply permutation to a fixed int32 column file in place (rewrite).
static void permute_i32(const std::string& path, uint64_t N, const std::vector<uint32_t>& perm) {
    auto src = read_i32_col(path, N);
    std::vector<int32_t> dst(N);
    for (uint64_t i = 0; i < N; ++i) dst[i] = src[perm[i]];
    write_bin(path, dst.data(), N * sizeof(int32_t));
}

// Apply permutation to a 1-byte column file.
static void permute_char1(const std::string& path, uint64_t N, const std::vector<uint32_t>& perm) {
    MMap mm;
    if (!mm.open(path)) std::exit(1);
    if (mm.size != N) { fprintf(stderr,"size mismatch char1 %s\n", path.c_str()); std::exit(1); }
    std::vector<char> dst(N);
    for (uint64_t i = 0; i < N; ++i) dst[i] = mm.data[perm[i]];
    write_bin(path, dst.data(), N);
}

// Apply permutation to a varlen column (offsets + data).
static void permute_varlen(const std::string& dir, const std::string& col, uint64_t N,
                           const std::vector<uint32_t>& perm) {
    std::string off_path = dir + "/" + col + ".offsets.bin";
    std::string dat_path = dir + "/" + col + ".data.bin";
    MMap omm, dmm;
    if (!omm.open(off_path) || !dmm.open(dat_path)) std::exit(1);
    if (omm.size != (N + 1) * sizeof(uint64_t)) {
        fprintf(stderr,"varlen offsets size mismatch %s: %zu vs %llu*8\n",
                off_path.c_str(), omm.size, (unsigned long long)(N+1));
        std::exit(1);
    }
    const uint64_t* old_off = (const uint64_t*)omm.data;
    const char* old_dat = dmm.data;

    std::vector<uint64_t> new_off(N + 1);
    new_off[0] = 0;
    for (uint64_t i = 0; i < N; ++i) {
        uint32_t r = perm[i];
        new_off[i+1] = new_off[i] + (old_off[r+1] - old_off[r]);
    }
    std::vector<char> new_dat(new_off[N]);
    for (uint64_t i = 0; i < N; ++i) {
        uint32_t r = perm[i];
        uint64_t s = old_off[r];
        uint64_t len = old_off[r+1] - s;
        if (len) std::memcpy(new_dat.data() + new_off[i], old_dat + s, len);
    }
    write_bin(off_path, new_off.data(), new_off.size() * sizeof(uint64_t));
    write_bin(dat_path, new_dat.data(), new_dat.size());
}

// Build an aux CSR (table is already sorted by some other column; we now build a secondary index
// on `col_path`). Counting sort against `col` produces rowids and offsets.
static void build_aux(const std::string& store, const std::string& table, const std::string& col) {
    auto t0 = std::chrono::steady_clock::now();
    std::string dir = store + "/" + table;
    uint64_t N = read_u64(dir + "/__row_count.bin");
    auto keys = read_i32_col(dir + "/" + col + ".bin", N);
    int32_t max_k = 0;
    for (auto v : keys) if (v > max_k) max_k = v;
    std::vector<uint64_t> off((size_t)max_k + 2, 0);
    for (uint64_t r = 0; r < N; ++r) {
        int32_t k = keys[r] < 0 ? 0 : keys[r];
        off[(size_t)k + 1]++;
    }
    for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];
    std::vector<int32_t> rowids(N);
    std::vector<uint64_t> cur = off;
    for (uint64_t r = 0; r < N; ++r) {
        int32_t k = keys[r] < 0 ? 0 : keys[r];
        rowids[cur[(size_t)k]++] = (int32_t)r;
    }
    fs::create_directories(store + "/indexes");
    std::string base = store + "/indexes/" + table + "__" + col;
    write_bin(base + "__offsets.bin", off.data(), off.size() * sizeof(uint64_t));
    write_bin(base + "__rowids.bin",  rowids.data(), N * sizeof(int32_t));
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[aux %s.%s] N=%llu max=%d  %.2fs\n",
            table.c_str(), col.c_str(), (unsigned long long)N, max_k, dt);
}

// Sort a fact table by sort_col and emit primary CSR index.
static void sort_and_index(const std::string& store, const TblJob& j) {
    auto t0 = std::chrono::steady_clock::now();
    std::string dir = store + "/" + j.name;
    uint64_t N = read_u64(dir + "/__row_count.bin");
    auto keys = read_i32_col(dir + "/" + j.sort_col + ".bin", N);

    std::vector<uint32_t> perm;
    std::vector<uint64_t> offsets;
    counting_sort(keys.data(), N, perm, offsets);

    // Free keys before heavy column rewrites.
    keys.clear(); keys.shrink_to_fit();

    // Rewrite each column according to permutation.
    for (const auto& c : j.int_cols) {
        permute_i32(dir + "/" + c + ".bin", N, perm);
    }
    for (const auto& c : j.char1_cols) {
        permute_char1(dir + "/" + c + ".bin", N, perm);
    }
    for (const auto& c : j.varlen_cols) {
        permute_varlen(dir, c, N, perm);
    }

    // Primary CSR (no rowids file — table is sorted by this key, so positions are contiguous).
    fs::create_directories(store + "/indexes");
    std::string idx_base = store + "/indexes/" + j.name + "__" + j.sort_col;
    write_bin(idx_base + "__offsets.bin", offsets.data(), offsets.size() * sizeof(uint64_t));

    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "[sort %s by %s] N=%llu max=%llu  %.2fs (%zu int + %zu char + %zu varlen cols)\n",
            j.name.c_str(), j.sort_col.c_str(),
            (unsigned long long)N, (unsigned long long)(offsets.size()-2), dt,
            j.int_cols.size(), j.char1_cols.size(), j.varlen_cols.size());
}

// ---------------- driver -----------------------------------------------------
int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr,"usage: %s <storage_dir>\n", argv[0]); return 1; }
    std::string store = argv[1];

    auto jobs = all_jobs();
    std::atomic<size_t> next{0};
    std::mutex log_mu;

    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;

    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size()) return;
            const auto& j = jobs[i];
            if (!j.pk_col.empty()) {
                build_pk_pos(store, j.name, j.pk_col);
            }
            if (!j.sort_col.empty()) {
                sort_and_index(store, j);
                for (const auto& a : j.aux) build_aux(store, j.name, a.col);
            }
        }
    };

    auto T0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < nthreads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    auto Twall = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
    fprintf(stderr, "[build_indexes] DONE wall=%.2fs threads=%u jobs=%zu\n",
            Twall, nthreads, jobs.size());
    return 0;
}
