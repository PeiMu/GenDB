// Q30c: IMDB-JOB
// Star query: complete_cast driver (cct1='cast', cct2='complete+verified') → distinct movie_id
// → semi-join movie_keyword (any of 7 keywords)
// → inner-join movie_info (it='genres', info in 6-genre set), tracks MIN(info)
// → inner-join movie_info_idx (it='votes'), tracks MIN(info)
// → inner-join cast_info (note in 5 writer-notes) → name (gender='m'), tracks MIN(name)
// MIN(title.title) over qualifying movies.

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <omp.h>

using namespace gendb;

static inline std::string csv_quote(std::string_view s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
    }
    if (!needs_quote) return std::string(s);
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

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    const std::string gd = argv[1];
    const std::string rd = argv[2];
    mkdir(rd.c_str(), 0755);

    // ---------------------------------------------------------------
    // mmap all required files
    // ---------------------------------------------------------------
    // dim resolution columns
    MmapColumn<int64_t> cct_kind_off (gd + "/comp_cast_type/kind.off");
    MmapColumn<char>    cct_kind_dat (gd + "/comp_cast_type/kind.dat");
    MmapColumn<int32_t> cct_id       (gd + "/comp_cast_type/id.bin");

    MmapColumn<int64_t> it_info_off  (gd + "/info_type/info.off");
    MmapColumn<char>    it_info_dat  (gd + "/info_type/info.dat");
    MmapColumn<int32_t> it_id        (gd + "/info_type/id.bin");

    MmapColumn<int64_t> k_kw_off     (gd + "/keyword/keyword.off");
    MmapColumn<char>    k_kw_dat     (gd + "/keyword/keyword.dat");
    MmapColumn<int32_t> k_id         (gd + "/keyword/id.bin");

    MmapColumn<int64_t> g_dict_off   (gd + "/name/gender.dict.off");
    MmapColumn<char>    g_dict_dat   (gd + "/name/gender.dict.dat");

    // driver
    MmapColumn<int32_t> cc_subject   (gd + "/complete_cast/subject_id.bin");
    MmapColumn<int32_t> cc_status    (gd + "/complete_cast/status_id.bin");
    MmapColumn<int32_t> cc_movie     (gd + "/complete_cast/movie_id.bin");

    // fact tables + their offsets indexes
    MmapColumn<int32_t> mk_off       (gd + "/_idx/movie_keyword__movie_id__offsets.bin");
    MmapColumn<int32_t> mk_kw        (gd + "/movie_keyword/keyword_id.bin");

    MmapColumn<int32_t> mi_off       (gd + "/_idx/movie_info__movie_id__offsets.bin");
    MmapColumn<int32_t> mi_iti       (gd + "/movie_info/info_type_id.bin");
    MmapColumn<int64_t> mi_info_off  (gd + "/movie_info/info.off");
    MmapColumn<char>    mi_info_dat  (gd + "/movie_info/info.dat");

    MmapColumn<int32_t> mx_off       (gd + "/_idx/movie_info_idx__movie_id__offsets.bin");
    MmapColumn<int32_t> mx_iti       (gd + "/movie_info_idx/info_type_id.bin");
    MmapColumn<int64_t> mx_info_off  (gd + "/movie_info_idx/info.off");
    MmapColumn<char>    mx_info_dat  (gd + "/movie_info_idx/info.dat");

    MmapColumn<int32_t> ci_off       (gd + "/_idx/cast_info__movie_id__offsets.bin");
    MmapColumn<int32_t> ci_person    (gd + "/cast_info/person_id.bin");
    MmapColumn<int64_t> ci_note_off  (gd + "/cast_info/note.off");
    MmapColumn<char>    ci_note_dat  (gd + "/cast_info/note.dat");

    MmapColumn<int8_t>  n_gender     (gd + "/name/gender.bin");
    MmapColumn<int64_t> n_name_off   (gd + "/name/name.off");
    MmapColumn<char>    n_name_dat   (gd + "/name/name.dat");

    MmapColumn<int64_t> t_title_off  (gd + "/title/title.off");
    MmapColumn<char>    t_title_dat  (gd + "/title/title.dat");

    // ---------------------------------------------------------------
    // Phase: data_loading — resolve dim constants
    // ---------------------------------------------------------------
    int32_t cct1_id = -1, cct2_id = -1;
    int32_t it1_id  = -1, it2_id  = -1;
    int8_t  m_code  = -1;
    std::unordered_set<int32_t> k_ids_set;

    {
        GENDB_PHASE("data_loading");

        for (size_t i = 0; i < cct_id.count; i++) {
            std::string_view k(cct_kind_dat.data + cct_kind_off[i],
                               cct_kind_off[i+1] - cct_kind_off[i]);
            if (k == "cast")              cct1_id = cct_id[i];
            else if (k == "complete+verified") cct2_id = cct_id[i];
        }
        for (size_t i = 0; i < it_id.count; i++) {
            std::string_view s(it_info_dat.data + it_info_off[i],
                               it_info_off[i+1] - it_info_off[i]);
            if (s == "genres") it1_id = it_id[i];
            else if (s == "votes")  it2_id = it_id[i];
        }
        static const char* k_lits[] = {
            "murder","violence","blood","gore","death","female-nudity","hospital"
        };
        for (size_t i = 0; i < k_id.count; i++) {
            std::string_view s(k_kw_dat.data + k_kw_off[i],
                               k_kw_off[i+1] - k_kw_off[i]);
            for (const char* lit : k_lits) {
                if (s == lit) { k_ids_set.insert(k_id[i]); break; }
            }
        }
        // gender dict: linear scan
        if (g_dict_off.count > 0) {
            size_t ndict = g_dict_off.count - 1;
            for (size_t i = 0; i < ndict; i++) {
                std::string_view s(g_dict_dat.data + g_dict_off[i],
                                   g_dict_off[i+1] - g_dict_off[i]);
                // Storage encodes dict codes as (i+1); code 0 is NULL/unknown.
                if (s == "m") { m_code = (int8_t)(i + 1); break; }
            }
        }
    }

    if (cct1_id < 0 || cct2_id < 0 || it1_id < 0 || it2_id < 0 || m_code < 0) {
        std::fprintf(stderr, "dim resolution failed: cct1=%d cct2=%d it1=%d it2=%d m_code=%d\n",
                     cct1_id, cct2_id, it1_id, it2_id, (int)m_code);
        return 1;
    }

    // ---------------------------------------------------------------
    // Phase: driver_scan — collect distinct movie_ids from complete_cast
    // ---------------------------------------------------------------
    std::vector<int32_t> mv_driver;
    {
        GENDB_PHASE("driver_scan");
        mv_driver.reserve(20000);
        size_t n = cc_subject.count;
        for (size_t i = 0; i < n; i++) {
            if (cc_subject[i] == cct1_id && cc_status[i] == cct2_id) {
                mv_driver.push_back(cc_movie[i]);
            }
        }
        std::sort(mv_driver.begin(), mv_driver.end());
        mv_driver.erase(std::unique(mv_driver.begin(), mv_driver.end()), mv_driver.end());
    }

    // genre / note literal sets
    std::unordered_set<std::string_view> genre_set = {
        "Horror","Action","Sci-Fi","Thriller","Crime","War"
    };
    std::unordered_set<std::string_view> note_set = {
        "(writer)","(head writer)","(written by)","(story)","(story editor)"
    };

    // ---------------------------------------------------------------
    // Phase: main_scan — per-mv index-nested-loop joins
    // ---------------------------------------------------------------
    struct LocalState {
        std::string min_mi_info, min_mi_idx_info, min_name, min_title;
        bool has = false;
    };
    int nthreads = omp_get_max_threads();
    std::vector<LocalState> locals(nthreads);

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel for schedule(dynamic, 32)
        for (size_t i = 0; i < mv_driver.size(); i++) {
            int tid = omp_get_thread_num();
            int32_t mv = mv_driver[i];

            // 1) movie_keyword semi-join: any keyword_id in k_ids_set
            int32_t lo, hi;
            lo = mk_off[mv]; hi = mk_off[mv+1];
            bool has_mk = false;
            for (int32_t r = lo; r < hi; r++) {
                if (k_ids_set.count(mk_kw[r])) { has_mk = true; break; }
            }
            if (!has_mk) continue;

            // 2) movie_info inner-join: it1_id AND info ∈ genre_set
            lo = mi_off[mv]; hi = mi_off[mv+1];
            std::string_view local_min_mi;
            bool has_mi = false;
            for (int32_t r = lo; r < hi; r++) {
                if (mi_iti[r] != it1_id) continue;
                int64_t a = mi_info_off[r], b = mi_info_off[r+1];
                int64_t len = b - a;
                if (len < 3 || len > 8) continue;  // length prefilter for genre set
                std::string_view info(mi_info_dat.data + a, len);
                if (genre_set.count(info)) {
                    if (!has_mi || info < local_min_mi) local_min_mi = info;
                    has_mi = true;
                }
            }
            if (!has_mi) continue;

            // 3) movie_info_idx inner-join: it2_id
            lo = mx_off[mv]; hi = mx_off[mv+1];
            std::string_view local_min_mx;
            bool has_mx = false;
            for (int32_t r = lo; r < hi; r++) {
                if (mx_iti[r] != it2_id) continue;
                int64_t a = mx_info_off[r], b = mx_info_off[r+1];
                std::string_view info(mx_info_dat.data + a, b - a);
                if (!has_mx || info < local_min_mx) local_min_mx = info;
                has_mx = true;
            }
            if (!has_mx) continue;

            // 4) cast_info inner-join: note in writer set AND name.gender==m
            lo = ci_off[mv]; hi = ci_off[mv+1];
            std::string_view local_min_name;
            bool has_ci = false;
            for (int32_t r = lo; r < hi; r++) {
                int64_t a = ci_note_off[r], b = ci_note_off[r+1];
                int64_t len = b - a;
                if (len < 6 || len > 15) continue;  // length prefilter for note set
                std::string_view note(ci_note_dat.data + a, len);
                if (!note_set.count(note)) continue;
                int32_t pid = ci_person[r];
                if (n_gender[pid - 1] != m_code) continue;
                int64_t na = n_name_off[pid - 1], nb = n_name_off[pid];
                std::string_view nm(n_name_dat.data + na, nb - na);
                if (!has_ci || nm < local_min_name) local_min_name = nm;
                has_ci = true;
            }
            if (!has_ci) continue;

            // qualifying movie — get title and update local mins
            int64_t ta = t_title_off[mv - 1], tb = t_title_off[mv];
            std::string_view ttl(t_title_dat.data + ta, tb - ta);

            LocalState& ls = locals[tid];
            if (!ls.has) {
                ls.min_mi_info.assign(local_min_mi.data(), local_min_mi.size());
                ls.min_mi_idx_info.assign(local_min_mx.data(), local_min_mx.size());
                ls.min_name.assign(local_min_name.data(), local_min_name.size());
                ls.min_title.assign(ttl.data(), ttl.size());
                ls.has = true;
            } else {
                if (local_min_mi < std::string_view(ls.min_mi_info))
                    ls.min_mi_info.assign(local_min_mi.data(), local_min_mi.size());
                if (local_min_mx < std::string_view(ls.min_mi_idx_info))
                    ls.min_mi_idx_info.assign(local_min_mx.data(), local_min_mx.size());
                if (local_min_name < std::string_view(ls.min_name))
                    ls.min_name.assign(local_min_name.data(), local_min_name.size());
                if (ttl < std::string_view(ls.min_title))
                    ls.min_title.assign(ttl.data(), ttl.size());
            }
        }
    }

    // ---------------------------------------------------------------
    // Reduce thread-local mins
    // ---------------------------------------------------------------
    std::string min_mi_info, min_mi_idx_info, min_name, min_title;
    bool has_result = false;
    for (int t = 0; t < nthreads; t++) {
        if (!locals[t].has) continue;
        if (!has_result) {
            min_mi_info     = locals[t].min_mi_info;
            min_mi_idx_info = locals[t].min_mi_idx_info;
            min_name        = locals[t].min_name;
            min_title       = locals[t].min_title;
            has_result = true;
        } else {
            if (locals[t].min_mi_info     < min_mi_info)     min_mi_info     = locals[t].min_mi_info;
            if (locals[t].min_mi_idx_info < min_mi_idx_info) min_mi_idx_info = locals[t].min_mi_idx_info;
            if (locals[t].min_name        < min_name)        min_name        = locals[t].min_name;
            if (locals[t].min_title       < min_title)       min_title       = locals[t].min_title;
        }
    }

    // ---------------------------------------------------------------
    // Phase: output
    // ---------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string outpath = rd + "/Q30c.csv";
        FILE* fp = std::fopen(outpath.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open output %s\n", outpath.c_str()); return 1; }
        std::fprintf(fp, "movie_budget,movie_votes,writer,complete_violent_movie\n");
        if (has_result) {
            std::fprintf(fp, "%s,%s,%s,%s\n",
                         csv_quote(min_mi_info).c_str(),
                         csv_quote(min_mi_idx_info).c_str(),
                         csv_quote(min_name).c_str(),
                         csv_quote(min_title).c_str());
        } else {
            std::fprintf(fp, ",,,\n");
        }
        std::fclose(fp);
    }
    return 0;
}
