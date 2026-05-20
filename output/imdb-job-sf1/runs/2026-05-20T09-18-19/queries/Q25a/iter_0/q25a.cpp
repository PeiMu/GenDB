// Q25a — GenDB iteration 0
// Generated implementation following the optimizer's plan.
//
// SELECT MIN(mi.info), MIN(mi_idx.info), MIN(n.name), MIN(t.title)
// FROM ... WHERE
//   ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
//   AND it1.info='genres' AND it2.info='votes'
//   AND k.keyword IN ('murder','blood','gore','death','female-nudity')
//   AND mi.info='Horror' AND n.gender='m'
//   AND join on movie_id (t,mi,mi_idx,ci,mk), n.id=ci.person_id,
//   it1.id=mi.info_type_id, it2.id=mi_idx.info_type_id, k.id=mk.keyword_id.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---- Helpers ----
static std::string_view view_off(const int64_t* off, const char* dat, int64_t i) {
    int64_t b = off[i], e = off[i + 1];
    return std::string_view(dat + b, (size_t)(e - b));
}

// CSV escape: surround with double quotes if contains comma, quote, or newline.
static std::string csv_escape(std::string_view s) {
    bool need = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
    }
    if (!need) return std::string(s);
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    GENDB_PHASE("total");

    // ---- Open all required columns (mmap) ----
    MmapColumn<int64_t> name_off, title_off, info_type_info_off, keyword_off,
                        mi_info_off, miidx_info_off, ci_note_off;
    MmapColumn<char>   name_dat, title_dat, info_type_info_dat, keyword_dat,
                       mi_info_dat, miidx_info_dat, ci_note_dat;
    MmapColumn<int32_t> info_type_id, keyword_id;
    MmapColumn<uint8_t> name_gender_bin;
    MmapColumn<char>    gender_dict_dat;
    MmapColumn<int64_t> gender_dict_off;

    MmapColumn<int32_t> mk_off, mi_off, miidx_off, ci_off;
    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> mi_info_type_id, miidx_info_type_id;
    MmapColumn<int32_t> ci_person_id;

    {
        GENDB_PHASE("data_loading");
        // dim files
        info_type_id.open(gendb_dir + "/info_type/id.bin");
        info_type_info_off.open(gendb_dir + "/info_type/info.off");
        info_type_info_dat.open(gendb_dir + "/info_type/info.dat");

        keyword_id.open(gendb_dir + "/keyword/id.bin");
        keyword_off.open(gendb_dir + "/keyword/keyword.off");
        keyword_dat.open(gendb_dir + "/keyword/keyword.dat");

        name_off.open(gendb_dir + "/name/name.off");
        name_dat.open(gendb_dir + "/name/name.dat");
        name_gender_bin.open(gendb_dir + "/name/gender.bin");
        gender_dict_dat.open(gendb_dir + "/name/gender.dict.dat");
        gender_dict_off.open(gendb_dir + "/name/gender.dict.off");

        title_off.open(gendb_dir + "/title/title.off");
        title_dat.open(gendb_dir + "/title/title.dat");

        // fact column data
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");

        miidx_info_off.open(gendb_dir + "/movie_info_idx/info.off");
        miidx_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");
        miidx_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");

        ci_note_off.open(gendb_dir + "/cast_info/note.off");
        ci_note_dat.open(gendb_dir + "/cast_info/note.dat");
        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");

        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        // offsets-only indexes
        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        miidx_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        ci_off.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");

        // Prefetch (HDD-friendly)
        mmap_prefetch_all(mi_info_type_id, miidx_info_type_id, mk_keyword_id, ci_person_id);
    }

    // ---- Resolve dimensions ----
    int32_t it1_id = -1, it2_id = -1;
    {
        GENDB_PHASE("resolve_info_type");
        size_t n = info_type_id.size();
        for (size_t i = 0; i < n; ++i) {
            auto v = view_off(info_type_info_off.data, info_type_info_dat.data, (int64_t)i);
            if (it1_id < 0 && v == std::string_view("genres")) it1_id = info_type_id[i];
            if (it2_id < 0 && v == std::string_view("votes"))  it2_id = info_type_id[i];
        }
        if (it1_id < 0 || it2_id < 0) {
            std::fprintf(stderr, "Failed to resolve info_type ids (it1=%d it2=%d)\n", it1_id, it2_id);
            return 2;
        }
    }

    // Resolve 5 keyword ids
    std::array<int32_t, 5> kw_ids{ -1, -1, -1, -1, -1 };
    int kw_found = 0;
    {
        GENDB_PHASE("resolve_keywords");
        static const std::array<std::string_view, 5> targets = {
            std::string_view("murder"),
            std::string_view("blood"),
            std::string_view("gore"),
            std::string_view("death"),
            std::string_view("female-nudity")
        };
        size_t n = keyword_id.size();
        for (size_t i = 0; i < n && kw_found < 5; ++i) {
            auto v = view_off(keyword_off.data, keyword_dat.data, (int64_t)i);
            for (int t = 0; t < 5; ++t) {
                if (kw_ids[t] < 0 && v == targets[t]) {
                    kw_ids[t] = keyword_id[i];
                    kw_found++;
                    break;
                }
            }
        }
    }

    // Resolve m_code. Per storage_design: code 0 = NULL, code i references dict entry i-1.
    uint8_t m_code = 255;
    {
        size_t nd = (gender_dict_off.size() > 0) ? gender_dict_off.size() - 1 : 0;
        for (size_t i = 0; i < nd; ++i) {
            auto v = view_off(gender_dict_off.data, gender_dict_dat.data, (int64_t)i);
            if (v == std::string_view("m")) { m_code = (uint8_t)(i + 1); break; }
        }
        if (m_code == 255) {
            std::fprintf(stderr, "Failed to resolve gender 'm' code\n");
            return 2;
        }
    }

    // Build valid_persons bitset over name (gender_bin == m_code)
    size_t n_rows = name_gender_bin.size();
    std::vector<uint64_t> valid_persons((n_rows + 63) / 64, 0);
    {
        GENDB_PHASE("build_valid_persons");
        const uint8_t* g = name_gender_bin.data;
        uint64_t* vp = valid_persons.data();
        for (size_t i = 0; i < n_rows; ++i) {
            if (g[i] == m_code) {
                vp[i >> 6] |= (uint64_t)1 << (i & 63);
            }
        }
    }
    auto check_person = [&](int32_t pid) -> bool {
        if (pid < 1 || (size_t)pid > n_rows) return false;
        size_t i = (size_t)(pid - 1);
        return (valid_persons[i >> 6] >> (i & 63)) & 1ULL;
    };

    // ---- Driver scan: title v=1..title_rows ----
    size_t title_rows = title_off.size() > 0 ? title_off.size() - 1 : 0;
    // offsets indexes have title_rows+2 entries indexed by v
    size_t off_count = mk_off.size();
    if (off_count != mi_off.size() || off_count != miidx_off.size() || off_count != ci_off.size()) {
        std::fprintf(stderr, "offsets size mismatch\n");
        return 3;
    }

    // Per-thread running mins.
    struct ThreadResult {
        std::string mi_info;
        std::string miidx_info;
        std::string name;
        std::string title;
        bool has = false;
    };

    int num_threads = (int)std::thread::hardware_concurrency();
    if (num_threads <= 0) num_threads = 12;
    if (num_threads > 24) num_threads = 24;

    const size_t MORSEL = 32768;
    std::atomic<size_t> next_morsel{1}; // movie ids start at 1
    std::vector<ThreadResult> tres(num_threads);

    auto worker = [&](int tid) {
        ThreadResult& R = tres[tid];
        // Local copies for hot pointers
        const int32_t* mk_o = mk_off.data;
        const int32_t* mi_o = mi_off.data;
        const int32_t* mii_o = miidx_off.data;
        const int32_t* ci_o = ci_off.data;
        const int32_t* mk_kw = mk_keyword_id.data;
        const int32_t* mi_iti = mi_info_type_id.data;
        const int32_t* mii_iti = miidx_info_type_id.data;
        const int64_t* mi_info_o = mi_info_off.data;
        const char*    mi_info_d = mi_info_dat.data;
        const int64_t* mii_info_o = miidx_info_off.data;
        const char*    mii_info_d = miidx_info_dat.data;
        const int64_t* ci_note_o = ci_note_off.data;
        const char*    ci_note_d = ci_note_dat.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int64_t* name_o = name_off.data;
        const char*    name_d = name_dat.data;
        const int64_t* title_o = title_off.data;
        const char*    title_d = title_dat.data;

        const int32_t kw0 = kw_ids[0], kw1 = kw_ids[1], kw2 = kw_ids[2],
                      kw3 = kw_ids[3], kw4 = kw_ids[4];
        const int32_t IT1 = it1_id;
        const int32_t IT2 = it2_id;

        // Writer-note set (5 literals)
        auto is_writer_note = [](std::string_view s) -> bool {
            switch (s.size()) {
                case 8:  // "(writer)"
                    return s == std::string_view("(writer)", 8);
                case 13: // "(head writer)"
                    return s == std::string_view("(head writer)", 13);
                case 12: // "(written by)"
                    return s == std::string_view("(written by)", 12);
                case 7:  // "(story)"
                    return s == std::string_view("(story)", 7);
                case 14: // "(story editor)"
                    return s == std::string_view("(story editor)", 14);
                default:
                    return false;
            }
        };

        while (true) {
            size_t start = next_morsel.fetch_add(MORSEL, std::memory_order_relaxed);
            if (start > title_rows) break;
            size_t end = start + MORSEL;
            if (end > title_rows + 1) end = title_rows + 1;

            for (size_t v = start; v < end; ++v) {
                // 1. movie_keyword: any row with keyword_id in kw_ids?
                int32_t lo = mk_o[v], hi = mk_o[v + 1];
                if (lo == hi) continue;
                bool mk_ok = false;
                for (int32_t r = lo; r < hi; ++r) {
                    int32_t kid = mk_kw[r];
                    if (kid == kw0 || kid == kw1 || kid == kw2 || kid == kw3 || kid == kw4) {
                        mk_ok = true;
                        break;
                    }
                }
                if (!mk_ok) continue;

                // 2. movie_info_idx: any row with info_type_id == IT2?
                lo = mii_o[v]; hi = mii_o[v + 1];
                if (lo == hi) continue;
                int32_t mii_hit_row = -1;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mii_iti[r] == IT2) { mii_hit_row = r; break; }
                }
                if (mii_hit_row < 0) continue;

                // 3. movie_info: any row with info_type_id == IT1 AND info == 'Horror'?
                lo = mi_o[v]; hi = mi_o[v + 1];
                if (lo == hi) continue;
                int32_t mi_hit_row = -1;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mi_iti[r] != IT1) continue;
                    int64_t b = mi_info_o[r], e = mi_info_o[r + 1];
                    if (e - b != 6) continue;
                    if (std::memcmp(mi_info_d + b, "Horror", 6) == 0) {
                        mi_hit_row = r;
                        break;
                    }
                }
                if (mi_hit_row < 0) continue;

                // 4. cast_info: note IN writer-set AND valid_persons[person_id-1]
                lo = ci_o[v]; hi = ci_o[v + 1];
                if (lo == hi) continue;
                int32_t ci_hit_row = -1;
                for (int32_t r = lo; r < hi; ++r) {
                    int64_t b = ci_note_o[r], e = ci_note_o[r + 1];
                    size_t len = (size_t)(e - b);
                    if (len != 7 && len != 8 && len != 12 && len != 13 && len != 14) continue;
                    std::string_view note(ci_note_d + b, len);
                    if (!is_writer_note(note)) continue;
                    int32_t pid = ci_pid[r];
                    if (!check_person(pid)) continue;
                    ci_hit_row = r;
                    break;
                }
                if (ci_hit_row < 0) continue;

                // All facts matched — compute / update running mins
                // mi.info (Horror; constant, but still track for MIN)
                {
                    int64_t b = mi_info_o[mi_hit_row], e = mi_info_o[mi_hit_row + 1];
                    std::string_view sv(mi_info_d + b, (size_t)(e - b));
                    if (!R.has || sv < std::string_view(R.mi_info)) R.mi_info.assign(sv);
                }
                // mi_idx.info
                {
                    int64_t b = mii_info_o[mii_hit_row], e = mii_info_o[mii_hit_row + 1];
                    std::string_view sv(mii_info_d + b, (size_t)(e - b));
                    if (!R.has || sv < std::string_view(R.miidx_info)) R.miidx_info.assign(sv);
                }
                // n.name via dense PK
                {
                    int32_t pid = ci_pid[ci_hit_row];
                    int64_t b = name_o[pid - 1], e = name_o[pid];
                    std::string_view sv(name_d + b, (size_t)(e - b));
                    if (!R.has || sv < std::string_view(R.name)) R.name.assign(sv);
                }
                // t.title
                {
                    int64_t b = title_o[v - 1], e = title_o[v];
                    std::string_view sv(title_d + b, (size_t)(e - b));
                    if (!R.has || sv < std::string_view(R.title)) R.title.assign(sv);
                }

                // For MIN we want the smallest across ALL surviving rows of each column,
                // not just the first row. Re-scan to find true minima for this movie.
                // (Cheap: only on movies that survive all filters.)
                // mi.info MIN over all qualifying rows of this movie
                {
                    int32_t lo2 = mi_o[v], hi2 = mi_o[v + 1];
                    for (int32_t r = lo2; r < hi2; ++r) {
                        if (mi_iti[r] != IT1) continue;
                        int64_t b = mi_info_o[r], e = mi_info_o[r + 1];
                        if (e - b != 6) continue;
                        if (std::memcmp(mi_info_d + b, "Horror", 6) != 0) continue;
                        std::string_view sv(mi_info_d + b, (size_t)(e - b));
                        if (sv < std::string_view(R.mi_info)) R.mi_info.assign(sv);
                    }
                }
                // mi_idx.info MIN over qualifying rows
                {
                    int32_t lo2 = mii_o[v], hi2 = mii_o[v + 1];
                    for (int32_t r = lo2; r < hi2; ++r) {
                        if (mii_iti[r] != IT2) continue;
                        int64_t b = mii_info_o[r], e = mii_info_o[r + 1];
                        std::string_view sv(mii_info_d + b, (size_t)(e - b));
                        if (sv < std::string_view(R.miidx_info)) R.miidx_info.assign(sv);
                    }
                }
                // n.name MIN over qualifying cast_info rows
                {
                    int32_t lo2 = ci_o[v], hi2 = ci_o[v + 1];
                    for (int32_t r = lo2; r < hi2; ++r) {
                        int64_t b = ci_note_o[r], e = ci_note_o[r + 1];
                        size_t len = (size_t)(e - b);
                        if (len != 7 && len != 8 && len != 12 && len != 13 && len != 14) continue;
                        std::string_view note(ci_note_d + b, len);
                        if (!is_writer_note(note)) continue;
                        int32_t pid = ci_pid[r];
                        if (!check_person(pid)) continue;
                        int64_t nb = name_o[pid - 1], ne = name_o[pid];
                        std::string_view sv(name_d + nb, (size_t)(ne - nb));
                        if (sv < std::string_view(R.name)) R.name.assign(sv);
                    }
                }
                // (title MIN already captured per-movie above)
                R.has = true;
            }
        }
    };

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> ths;
        ths.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) ths.emplace_back(worker, t);
        for (auto& t : ths) t.join();
    }

    // Reduce thread-local mins
    ThreadResult G;
    for (int t = 0; t < num_threads; ++t) {
        if (!tres[t].has) continue;
        if (!G.has) {
            G = std::move(tres[t]);
            G.has = true;
        } else {
            if (std::string_view(tres[t].mi_info)   < std::string_view(G.mi_info))   G.mi_info   = tres[t].mi_info;
            if (std::string_view(tres[t].miidx_info)< std::string_view(G.miidx_info))G.miidx_info= tres[t].miidx_info;
            if (std::string_view(tres[t].name)     < std::string_view(G.name))      G.name      = tres[t].name;
            if (std::string_view(tres[t].title)    < std::string_view(G.title))     G.title     = tres[t].title;
        }
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::filesystem::create_directories(results_dir);
        std::string out_path = results_dir + "/Q25a.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Cannot open %s\n", out_path.c_str()); return 4; }
        std::fprintf(f, "movie_budget,movie_votes,male_writer,violent_movie_title\n");
        if (G.has) {
            std::string c1 = csv_escape(G.mi_info);
            std::string c2 = csv_escape(G.miidx_info);
            std::string c3 = csv_escape(G.name);
            std::string c4 = csv_escape(G.title);
            std::fprintf(f, "%s,%s,%s,%s\n", c1.c_str(), c2.c_str(), c3.c_str(), c4.c_str());
        } else {
            std::fprintf(f, ",,,\n");
        }
        std::fclose(f);
    }

    return 0;
}
