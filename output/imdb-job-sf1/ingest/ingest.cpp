// IMDB JOB ingestion: CSV -> binary columnar files.
// Tables are parsed in parallel using a work queue (one table per worker).

#include "csv_common.h"

#include <algorithm>
#include <chrono>

// --- table schema declarations ------------------------------------------------

static std::vector<TableSpec> all_tables() {
    std::vector<TableSpec> ts;

    auto I  = [](const char* n) { return make_col(ColType::INT32, n); };
    auto IN = [](const char* n) { return make_col(ColType::INT32_NULL, n); };
    auto V  = [](const char* n) { return make_col(ColType::VARLEN, n); };
    auto CN = [](const char* n) { return make_col(ColType::CHAR1_NULL, n); };

    auto add = [&](std::string name, std::string file,
                   std::vector<std::string> csv,
                   std::vector<ColBuf> out) {
        ts.emplace_back();
        ts.back().name = std::move(name);
        ts.back().csv_filename = std::move(file);
        ts.back().csv_cols = std::move(csv);
        ts.back().out_cols = std::move(out);
    };

    // ---- dimension tables (small) ----
    add("info_type",      "info_type.csv",      {"id","info"},                                                                                    { I("id"), V("info") });
    add("link_type",      "link_type.csv",      {"id","link"},                                                                                    { I("id"), V("link") });
    add("role_type",      "role_type.csv",      {"id","role"},                                                                                    { I("id"), V("role") });
    add("kind_type",      "kind_type.csv",      {"id","kind"},                                                                                    { I("id"), V("kind") });
    add("company_type",   "company_type.csv",   {"id","kind"},                                                                                    { I("id"), V("kind") });
    add("comp_cast_type", "comp_cast_type.csv", {"id","kind"},                                                                                    { I("id"), V("kind") });

    // ---- dimension PK tables (medium/large) ----
    add("keyword",        "keyword.csv",        {"id","keyword","phonetic_code"},                                                                  { I("id"), V("keyword") });
    add("company_name",   "company_name.csv",   {"id","name","country_code","imdb_id","name_pcode_nf","name_pcode_sf","md5sum"},                   { I("id"), V("name"), V("country_code") });
    add("char_name",      "char_name.csv",      {"id","name","imdb_index","imdb_id","name_pcode_nf","surname_pcode","md5sum"},                     { I("id"), V("name") });
    add("name",           "name.csv",           {"id","name","imdb_index","imdb_id","gender","name_pcode_cf","name_pcode_nf","surname_pcode","md5sum"}, { I("id"), V("name"), CN("gender"), V("name_pcode_cf") });
    add("title",          "title.csv",          {"id","title","imdb_index","kind_id","production_year","imdb_id","phonetic_code","episode_of_id","season_nr","episode_nr","series_years","md5sum"},
                                                                                                                                                  { I("id"), V("title"), I("kind_id"), IN("production_year"), IN("episode_nr") });

    // ---- fact tables ----
    add("aka_name",       "aka_name.csv",       {"id","person_id","name","imdb_index","name_pcode_cf","name_pcode_nf","surname_pcode","md5sum"},   { I("person_id"), V("name") });
    add("aka_title",      "aka_title.csv",      {"id","movie_id","title","imdb_index","kind_id","production_year","phonetic_code","episode_of_id","season_nr","episode_nr","note","md5sum"},
                                                                                                                                                  { I("movie_id"), V("title") });
    add("complete_cast",  "complete_cast.csv",  {"id","movie_id","subject_id","status_id"},                                                        { IN("movie_id"), I("subject_id"), I("status_id") });
    add("movie_link",     "movie_link.csv",     {"id","movie_id","linked_movie_id","link_type_id"},                                                { I("movie_id"), I("linked_movie_id"), I("link_type_id") });
    add("movie_companies","movie_companies.csv",{"id","movie_id","company_id","company_type_id","note"},                                           { I("movie_id"), I("company_id"), I("company_type_id"), V("note") });
    add("movie_keyword",  "movie_keyword.csv",  {"id","movie_id","keyword_id"},                                                                    { I("movie_id"), I("keyword_id") });
    add("movie_info_idx", "movie_info_idx.csv", {"id","movie_id","info_type_id","info","note"},                                                    { I("movie_id"), I("info_type_id"), V("info") });
    add("person_info",    "person_info.csv",    {"id","person_id","info_type_id","info","note"},                                                   { I("person_id"), I("info_type_id"), V("info"), V("note") });
    add("movie_info",     "movie_info.csv",     {"id","movie_id","info_type_id","info","note"},                                                    { I("movie_id"), I("info_type_id"), V("info"), V("note") });
    add("cast_info",      "cast_info.csv",      {"id","person_id","movie_id","person_role_id","note","nr_order","role_id"},                        { I("person_id"), I("movie_id"), IN("person_role_id"), V("note"), I("role_id") });

    for (auto& t : ts) prep_table(t);
    return ts;
}

// --- worker pool --------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <csv_dir> <storage_dir>\n", argv[0]);
        return 1;
    }
    std::string csv_dir = argv[1];
    std::string storage_dir = argv[2];
    fs::create_directories(storage_dir);

    auto tables = all_tables();

    // Estimate work by CSV file size; schedule largest-first (LPT).
    std::vector<size_t> file_sizes(tables.size(), 0);
    for (size_t i = 0; i < tables.size(); ++i) {
        struct stat st;
        std::string p = csv_dir + "/" + tables[i].csv_filename;
        if (stat(p.c_str(), &st) == 0) file_sizes[i] = (size_t)st.st_size;
    }
    std::vector<size_t> order(tables.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b){ return file_sizes[a] > file_sizes[b]; });

    std::atomic<size_t> next{0};
    std::mutex log_mu;

    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= order.size()) return;
            size_t ti = order[i];
            auto& t = tables[ti];
            auto t0 = std::chrono::steady_clock::now();
            uint64_t rc = parse_table(t, csv_dir);
            write_table(t, rc, storage_dir);
            auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> lk(log_mu);
                std::fprintf(stderr, "[ingest] %-18s rows=%-10llu  %.2fs  (%.1f MB)\n",
                             t.name.c_str(), (unsigned long long)rc, dt, file_sizes[ti] / 1048576.0);
            }
        }
    };

    unsigned int nthreads = std::min<unsigned int>(std::thread::hardware_concurrency(), (unsigned)tables.size());
    if (nthreads == 0) nthreads = 4;
    std::vector<std::thread> pool;
    auto T0 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < nthreads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    auto Twall = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
    std::fprintf(stderr, "[ingest] DONE  wall=%.2fs threads=%u tables=%zu\n", Twall, nthreads, tables.size());
    return 0;
}
