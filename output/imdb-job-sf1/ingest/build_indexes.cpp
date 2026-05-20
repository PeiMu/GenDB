// Build join indexes for IMDB-JOB SF1.
//   offsets_only: written when the child table is primarily sorted by `fkcol`.
//                 file: <storage>/_idx/<child>__<fkcol>__offsets.bin
//                 layout: int32 array of size (max_parent_id+2). Children for
//                         parent_id=v live in [arr[v], arr[v+1]). Slot 0 holds
//                         the count of rows with fkcol < 1 (NULLs / out-of-range).
//   csr:          two files: __offsets.bin (same layout as above) and
//                 __rowids.bin (int32 N_child, child row indices ordered by fkcol).
//
// All offsets and row IDs are int32 because no child table exceeds 2^31 rows.

#include "common.h"
#include <atomic>
#include <functional>

static std::string IDX_DIR;  // <storage>/_idx

static std::vector<int32_t> load_int32(const std::string& path) {
    return read_vec<int32_t>(path);
}

// Build offsets[v] = first index i where fk[i] >= v, for v in [0..max_parent+1].
// Assumes fk is sorted ascending.  fk values <= 0 (NULL_INT or unknowns) sort
// to the front; offsets[1] tells the planner where v=1 starts.
static void build_offsets_sorted(const std::vector<int32_t>& fk, int32_t maxid,
                                 const std::string& name) {
    std::vector<int32_t> off(maxid + 2, (int32_t)fk.size());
    // Walk fk; whenever fk[i] != prev_val advance off for all v in (prev_val..fk[i]].
    // Simpler two-pointer: for v=0..maxid+1, advance i until fk[i] >= v.
    int32_t i = 0;
    for (int32_t v = 0; v <= maxid + 1; ++v) {
        while (i < (int32_t)fk.size() && fk[i] < v) ++i;
        off[v] = i;
    }
    write_vec(IDX_DIR + "/" + name + "__offsets.bin", off);
    logmsg("[idx] %s offsets size=%d (max=%d)", name.c_str(), (int)off.size(), maxid);
}

// Build CSR: row IDs sorted stably by fk; offsets[v]=first row in CSR with fk>=v.
static void build_csr(const std::vector<int32_t>& fk, int32_t maxid,
                      const std::string& name) {
    const size_t N = fk.size();
    // Counting sort on fk (range 0..maxid). NULL_INT (negative) goes to bucket 0
    // by clamping to 0; planner can skip slot 0 since real parent ids start at 1.
    std::vector<int32_t> count(maxid + 2, 0);
    for (size_t i = 0; i < N; ++i) {
        int32_t v = fk[i];
        if (v < 0) v = 0;
        if (v > maxid) v = maxid + 1;  // out-of-range, parked at end bucket
        count[v]++;
    }
    std::vector<int32_t> off(maxid + 2, 0);
    int32_t running = 0;
    for (int32_t v = 0; v <= maxid + 1; ++v) {
        off[v] = running;
        running += count[v];
    }
    // off[v] now points to start of bucket v. Append maxid+2 sentinel.
    std::vector<int32_t> rowids(N);
    std::vector<int32_t> cursor = off;  // copy as cursor
    for (int32_t i = 0; i < (int32_t)N; ++i) {
        int32_t v = fk[i];
        if (v < 0) v = 0;
        if (v > maxid) v = maxid + 1;
        rowids[cursor[v]++] = i;
    }
    // Offsets array we expose to queries: size maxid+2 where slot v means
    // "first CSR position with fkcol >= v". slot 0 starts the NULL bucket;
    // slot 1 is where parent_id=1 rows begin; slot maxid+1 is one past the
    // last valid parent.
    std::vector<int32_t> off_out(maxid + 2);
    for (int32_t v = 0; v <= maxid + 1; ++v) off_out[v] = off[v];
    write_vec(IDX_DIR + "/" + name + "__offsets.bin", off_out);
    write_vec(IDX_DIR + "/" + name + "__rowids.bin",  rowids);
    logmsg("[idx] %s CSR rows=%zu maxid=%d", name.c_str(), N, maxid);
}

// ---- Index tasks: kept as lambdas to run in a small thread pool. ------------

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <storage_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    IDX_DIR = store + "/_idx";
    mkdirp(IDX_DIR);

    using Task = std::function<void()>;
    std::vector<Task> tasks;

    // ---- offsets-only (child sorted by fkcol) -------------------------------
    auto add_offsets = [&](const char* child, const char* fkcol, int32_t parent_max) {
        std::string colpath = store + "/" + child + "/" + fkcol + ".bin";
        std::string idx = std::string(child) + "__" + fkcol;
        tasks.push_back([colpath, parent_max, idx]{
            auto fk = load_int32(colpath);
            build_offsets_sorted(fk, parent_max, idx);
        });
    };
    add_offsets("cast_info",        "movie_id",  2528312);
    add_offsets("movie_info",       "movie_id",  2528312);
    add_offsets("movie_info_idx",   "movie_id",  2528312);
    add_offsets("movie_keyword",    "movie_id",  2528312);
    add_offsets("movie_companies",  "movie_id",  2528312);
    add_offsets("movie_link",       "movie_id",  2528312);
    add_offsets("complete_cast",    "movie_id",  2528312);
    add_offsets("aka_title",        "movie_id",  2528312);
    add_offsets("aka_name",         "person_id", 4167491);
    add_offsets("person_info",      "person_id", 4167491);

    // ---- CSR (secondary indexes) --------------------------------------------
    auto add_csr = [&](const char* child, const char* fkcol, int32_t parent_max) {
        std::string colpath = store + "/" + child + "/" + fkcol + ".bin";
        std::string idx = std::string(child) + "__" + fkcol;
        tasks.push_back([colpath, parent_max, idx]{
            auto fk = load_int32(colpath);
            build_csr(fk, parent_max, idx);
        });
    };
    add_csr("cast_info",       "person_id",       4167491);
    add_csr("cast_info",       "person_role_id",  3140339);
    add_csr("cast_info",       "role_id",         12);
    add_csr("movie_keyword",   "keyword_id",      134170);
    add_csr("movie_info",      "info_type_id",    113);
    add_csr("movie_info_idx",  "info_type_id",    113);
    add_csr("person_info",     "info_type_id",    113);
    add_csr("movie_companies", "company_id",      234997);
    add_csr("movie_companies", "company_type_id", 4);
    add_csr("movie_link",      "link_type_id",    18);
    add_csr("movie_link",      "linked_movie_id", 2528312);
    add_csr("complete_cast",   "subject_id",      4);
    add_csr("complete_cast",   "status_id",       4);
    add_csr("title",           "kind_id",         7);

    // ---- Run tasks across worker threads ------------------------------------
    std::atomic<size_t> next{0};
    int nthreads = std::min<int>(12, (int)tasks.size());
    std::vector<std::thread> workers;
    for (int t = 0; t < nthreads; ++t) {
        workers.emplace_back([&]{
            while (true) {
                size_t i = next.fetch_add(1);
                if (i >= tasks.size()) return;
                tasks[i]();
            }
        });
    }
    for (auto& w : workers) w.join();
    logmsg("indexes complete");
    return 0;
}
