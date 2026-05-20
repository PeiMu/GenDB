// Q26a — IMDb JOB
// MIN aggregation over title-driven star join.
// Driver: title rows.
// Probes: movie_keyword, complete_cast, movie_info_idx, cast_info via offsets indexes.
// Aggregation: 4 string MINs (chn.name, mi_idx.info, n.name, t.title).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using gendb::MmapColumn;
using std::string_view;

static constexpr int32_t NULL_INT = INT32_MIN;

// --------- helpers ---------
struct VarlenCol {
    MmapColumn<uint64_t> off;
    MmapColumn<char>     dat;
    size_t rows() const { return off.count - 1; }
    string_view get(size_t i) const {
        uint64_t s = off[i], e = off[i + 1];
        return string_view(dat.data + s, e - s);
    }
    bool is_null(size_t i) const {
        return off[i] == off[i + 1];
    }
};

static void open_varlen(VarlenCol& v, const std::string& base) {
    v.off.open(base + ".off");
    v.dat.open(base + ".dat");
}

static inline bool memmem_fast(const char* hay, size_t hl, const char* needle, size_t nl) {
    if (nl == 0) return true;
    if (hl < nl) return false;
    return memmem(hay, hl, needle, nl) != nullptr;
}

static void write_csv_field(std::string& out, string_view s) {
    bool need_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        out.append(s.data(), s.size());
    } else {
        out.push_back('"');
        for (char c : s) {
            if (c == '"') out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
    }
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // ---------------- Data loading ----------------
    // Driver: title
    MmapColumn<int32_t> t_kind_id, t_prod_year;
    VarlenCol           t_title;

    // Indexes (offsets)
    MmapColumn<int32_t> mk_off, cc_off, mi_off, ci_off;

    // Fact columns
    MmapColumn<int32_t> mk_keyword_id;
    MmapColumn<int32_t> cc_subject_id, cc_status_id;
    MmapColumn<int32_t> mi_info_type_id;
    VarlenCol           mi_info;
    MmapColumn<int32_t> ci_person_role_id, ci_person_id;

    // Dim columns
    VarlenCol cct_kind, it_info, kt_kind, kw_kw, chn_name, n_name;

    {
        GENDB_PHASE("data_loading");
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_prod_year.open(gendb_dir + "/title/production_year.bin");
        open_varlen(t_title, gendb_dir + "/title/title");

        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        cc_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        mi_off.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        ci_off.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");

        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");
        cc_subject_id.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");
        mi_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        open_varlen(mi_info, gendb_dir + "/movie_info_idx/info");
        ci_person_role_id.open(gendb_dir + "/cast_info/person_role_id.bin");
        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");

        open_varlen(cct_kind, gendb_dir + "/comp_cast_type/kind");
        open_varlen(it_info, gendb_dir + "/info_type/info");
        open_varlen(kt_kind, gendb_dir + "/kind_type/kind");
        open_varlen(kw_kw,  gendb_dir + "/keyword/keyword");
        open_varlen(chn_name, gendb_dir + "/char_name/name");
        open_varlen(n_name,   gendb_dir + "/name/name");
    }

    // ---------------- Build dim sets ----------------
    int32_t cast_id = -1;
    int32_t complete_status_ids[8];
    int     complete_n = 0;
    int32_t it2_id = -1;
    int32_t movie_kind_id = -1;
    std::unordered_set<int32_t> kw_set;
    std::vector<uint64_t> chn_bits; // bitset over chn_id
    size_t chn_rows = chn_name.rows();

    {
        GENDB_PHASE("build_dim_sets");

        // comp_cast_type: id = row+1
        size_t cct_rows = cct_kind.rows();
        for (size_t i = 0; i < cct_rows; i++) {
            string_view s = cct_kind.get(i);
            int32_t id = (int32_t)(i + 1);
            if (s == "cast") cast_id = id;
            if (memmem_fast(s.data(), s.size(), "complete", 8)) {
                if (complete_n < 8) complete_status_ids[complete_n++] = id;
            }
        }

        // info_type
        size_t it_rows = it_info.rows();
        for (size_t i = 0; i < it_rows; i++) {
            string_view s = it_info.get(i);
            if (s == "rating") { it2_id = (int32_t)(i + 1); break; }
        }

        // kind_type
        size_t kt_rows = kt_kind.rows();
        for (size_t i = 0; i < kt_rows; i++) {
            string_view s = kt_kind.get(i);
            if (s == "movie") { movie_kind_id = (int32_t)(i + 1); break; }
        }

        // keyword set: 10 literals
        static const char* kw_literals[10] = {
            "superhero","marvel-comics","based-on-comic","tv-special",
            "fight","violence","magnet","web","claw","laser"
        };
        std::unordered_set<string_view> kw_lit_set;
        for (auto* p : kw_literals) kw_lit_set.insert(p);
        kw_set.reserve(16);
        size_t kw_rows = kw_kw.rows();
        for (size_t i = 0; i < kw_rows; i++) {
            string_view s = kw_kw.get(i);
            if (kw_lit_set.count(s)) kw_set.insert((int32_t)(i + 1));
        }

        // chn_set bitset: chn_id ∈ [1..chn_rows]
        chn_bits.assign((chn_rows + 64) / 64 + 1, 0ull);
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < chn_rows; i++) {
            uint64_t s = chn_name.off[i], e = chn_name.off[i + 1];
            if (s == e) continue; // NULL
            const char* d = chn_name.dat.data + s;
            size_t l = e - s;
            if (memmem_fast(d, l, "man", 3) || memmem_fast(d, l, "Man", 3)) {
                int32_t id = (int32_t)(i + 1);
                size_t word = id >> 6;
                uint64_t bit = 1ull << (id & 63);
                #pragma omp atomic
                chn_bits[word] |= bit;
            }
        }
    }

    if (cast_id < 0 || it2_id < 0 || movie_kind_id < 0 || complete_n == 0) {
        std::fprintf(stderr, "Failed to resolve dim scalars\n");
        return 1;
    }

    auto chn_test = [&chn_bits](int32_t id) -> bool {
        if (id <= 0) return false;
        size_t word = (size_t)id >> 6;
        uint64_t bit = 1ull << (id & 63);
        return (chn_bits[word] & bit) != 0;
    };

    // ---------------- Main scan ----------------
    size_t t_rows = t_kind_id.count;

    // Thread-local mins (4 strings). Use std::string to own data.
    int nthreads = omp_get_max_threads();
    std::vector<std::string> tl_chn(nthreads), tl_mi(nthreads), tl_n(nthreads), tl_t(nthreads);
    std::vector<bool> tl_has(nthreads, false);

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string& best_chn = tl_chn[tid];
            std::string& best_mi  = tl_mi[tid];
            std::string& best_n   = tl_n[tid];
            std::string& best_t   = tl_t[tid];
            bool has_any = false;

            // Local strings for this title's best candidates
            std::string cand_mi, cand_chn, cand_n;

            #pragma omp for schedule(dynamic, 4096)
            for (size_t r = 0; r < t_rows; r++) {
                // Filter title
                int32_t k = t_kind_id[r];
                if (k != movie_kind_id) continue;
                int32_t py = t_prod_year[r];
                if (py == NULL_INT || py <= 2000) continue;

                int32_t t_id = (int32_t)(r + 1);

                // Probe movie_keyword (semi)
                int32_t mk_lo = mk_off[t_id], mk_hi = mk_off[t_id + 1];
                if (mk_lo >= mk_hi) continue;
                bool mk_match = false;
                for (int32_t j = mk_lo; j < mk_hi; j++) {
                    int32_t kid = mk_keyword_id[j];
                    if (kw_set.count(kid)) { mk_match = true; break; }
                }
                if (!mk_match) continue;

                // Probe complete_cast (semi)
                int32_t cc_lo = cc_off[t_id], cc_hi = cc_off[t_id + 1];
                if (cc_lo >= cc_hi) continue;
                bool cc_match = false;
                for (int32_t j = cc_lo; j < cc_hi; j++) {
                    if (cc_subject_id[j] != cast_id) continue;
                    int32_t st = cc_status_id[j];
                    bool ok = false;
                    for (int p = 0; p < complete_n; p++) {
                        if (complete_status_ids[p] == st) { ok = true; break; }
                    }
                    if (ok) { cc_match = true; break; }
                }
                if (!cc_match) continue;

                // Probe movie_info_idx (semi + min(info))
                int32_t mi_lo = mi_off[t_id], mi_hi = mi_off[t_id + 1];
                if (mi_lo >= mi_hi) continue;
                cand_mi.clear();
                bool mi_found = false;
                for (int32_t j = mi_lo; j < mi_hi; j++) {
                    if (mi_info_type_id[j] != it2_id) continue;
                    string_view inf = mi_info.get(j);
                    if (!(inf > string_view("7.0"))) continue;
                    if (!mi_found || inf < string_view(cand_mi)) {
                        cand_mi.assign(inf.data(), inf.size());
                        mi_found = true;
                    }
                }
                if (!mi_found) continue;

                // Probe cast_info (inner + project chn.name + n.name)
                int32_t ci_lo = ci_off[t_id], ci_hi = ci_off[t_id + 1];
                if (ci_lo >= ci_hi) continue;
                cand_chn.clear();
                cand_n.clear();
                bool ci_found = false;
                for (int32_t j = ci_lo; j < ci_hi; j++) {
                    int32_t prid = ci_person_role_id[j];
                    if (prid == NULL_INT) continue;
                    if (!chn_test(prid)) continue;
                    int32_t pid = ci_person_id[j];
                    if (pid == NULL_INT || pid <= 0) continue;
                    string_view cn = chn_name.get((size_t)prid - 1);
                    string_view nn = n_name.get((size_t)pid - 1);
                    if (!ci_found || cn < string_view(cand_chn)) {
                        cand_chn.assign(cn.data(), cn.size());
                    }
                    if (!ci_found || nn < string_view(cand_n)) {
                        cand_n.assign(nn.data(), nn.size());
                    }
                    ci_found = true;
                }
                if (!ci_found) continue;

                // All four conditions hold; t.title is candidate.
                string_view tt = t_title.get(r);

                if (!has_any) {
                    best_chn.assign(cand_chn);
                    best_mi.assign(cand_mi);
                    best_n.assign(cand_n);
                    best_t.assign(tt.data(), tt.size());
                    has_any = true;
                } else {
                    if (cand_chn < best_chn) best_chn = cand_chn;
                    if (cand_mi  < best_mi)  best_mi  = cand_mi;
                    if (cand_n   < best_n)   best_n   = cand_n;
                    if (string_view(tt) < string_view(best_t))
                        best_t.assign(tt.data(), tt.size());
                }
            }

            tl_has[tid] = has_any;
        }
    }

    // Reduce
    std::string g_chn, g_mi, g_n, g_t;
    bool g_has = false;
    for (int i = 0; i < nthreads; i++) {
        if (!tl_has[i]) continue;
        if (!g_has) {
            g_chn = tl_chn[i];
            g_mi  = tl_mi[i];
            g_n   = tl_n[i];
            g_t   = tl_t[i];
            g_has = true;
        } else {
            if (tl_chn[i] < g_chn) g_chn = tl_chn[i];
            if (tl_mi[i]  < g_mi)  g_mi  = tl_mi[i];
            if (tl_n[i]   < g_n)   g_n   = tl_n[i];
            if (tl_t[i]   < g_t)   g_t   = tl_t[i];
        }
    }

    // ---------------- Output ----------------
    {
        GENDB_PHASE("output");
        std::string outpath = results_dir + "/Q26a.csv";
        std::ofstream ofs(outpath);
        ofs << "character_name,rating,playing_actor,complete_hero_movie\n";
        if (g_has) {
            std::string line;
            write_csv_field(line, g_chn); line.push_back(',');
            write_csv_field(line, g_mi);  line.push_back(',');
            write_csv_field(line, g_n);   line.push_back(',');
            write_csv_field(line, g_t);   line.push_back('\n');
            ofs << line;
        }
        ofs.close();
    }

    return 0;
}
