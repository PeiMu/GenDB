// Q28b — IMDB-JOB
// MIN(cn.name), MIN(mi_idx.info), MIN(t.title)
// Driver: title (year>2005, kind_id ∈ {movie,episode})
// Probe order: mk -> cc -> mi_idx -> mc -> mi (selectivity ascending)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;
namespace fs = std::filesystem;

// Helpers ------------------------------------------------------------------

struct Varlen {
    const int64_t* off;   // size n+1
    const char*    dat;
    MmapColumn<int64_t> off_col;
    MmapColumn<char>    dat_col;
    void load(const std::string& base) {
        off_col.open(base + ".off");
        dat_col.open(base + ".dat");
        off = off_col.data;
        dat = dat_col.data;
    }
    std::string_view at(size_t row) const {
        int64_t a = off[row], b = off[row + 1];
        return std::string_view(dat + a, b - a);
    }
    bool is_null(size_t row) const {
        return off[row + 1] == off[row];
    }
    size_t count() const { return off_col.count - 1; }
};

// Search for needle in haystack; return position or std::string_view::npos
static inline size_t sv_find(std::string_view hay, const char* needle, size_t nlen) {
    if (nlen == 0) return 0;
    if (hay.size() < nlen) return std::string_view::npos;
    const char* h = hay.data();
    size_t n = hay.size();
    for (size_t i = 0; i + nlen <= n; ++i) {
        if (std::memcmp(h + i, needle, nlen) == 0) return i;
    }
    return std::string_view::npos;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir   = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    // ---------------- DATA LOADING ----------------
    // Map all required columns
    MmapColumn<int32_t> t_id, t_kind_id, t_prod_year;
    Varlen              t_title;

    MmapColumn<int32_t> kt_id;
    Varlen              kt_kind;

    MmapColumn<int32_t> cct_id;
    Varlen              cct_kind;

    MmapColumn<int32_t> it_id;
    Varlen              it_info;

    MmapColumn<int32_t> kw_id;
    Varlen              kw_kw;

    MmapColumn<int32_t> cn_id;
    MmapColumn<int16_t> cn_cc;        // dict-encoded country code
    MmapColumn<int64_t> cn_cc_doff;   // dict offsets
    MmapColumn<char>    cn_cc_ddat;   // dict bytes
    Varlen              cn_name;

    MmapColumn<int32_t> mc_mid, mc_cid, mc_ctid;
    Varlen              mc_note;

    MmapColumn<int32_t> mk_mid, mk_kid;

    MmapColumn<int32_t> cc_mid, cc_subj, cc_stat;

    MmapColumn<int32_t> mi_mid, mi_itid;
    Varlen              mi_info;

    MmapColumn<int32_t> mix_mid, mix_itid;
    Varlen              mix_info;

    // indexes (offsets_only)
    MmapColumn<int32_t> idx_mk, idx_cc, idx_mix, idx_mc, idx_mi;

    {
        GENDB_PHASE("data_loading");

        t_id.open(gendb_dir + "/title/id.bin");
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_prod_year.open(gendb_dir + "/title/production_year.bin");
        t_title.load(gendb_dir + "/title/title");

        kt_id.open(gendb_dir + "/kind_type/id.bin");
        kt_kind.load(gendb_dir + "/kind_type/kind");

        cct_id.open(gendb_dir + "/comp_cast_type/id.bin");
        cct_kind.load(gendb_dir + "/comp_cast_type/kind");

        it_id.open(gendb_dir + "/info_type/id.bin");
        it_info.load(gendb_dir + "/info_type/info");

        kw_id.open(gendb_dir + "/keyword/id.bin");
        kw_kw.load(gendb_dir + "/keyword/keyword");

        cn_id.open(gendb_dir + "/company_name/id.bin");
        cn_cc.open(gendb_dir + "/company_name/country_code.bin");
        cn_cc_doff.open(gendb_dir + "/company_name/country_code.dict.off");
        cn_cc_ddat.open(gendb_dir + "/company_name/country_code.dict.dat");
        cn_name.load(gendb_dir + "/company_name/name");

        mc_mid.open(gendb_dir + "/movie_companies/movie_id.bin");
        mc_cid.open(gendb_dir + "/movie_companies/company_id.bin");
        mc_ctid.open(gendb_dir + "/movie_companies/company_type_id.bin");
        mc_note.load(gendb_dir + "/movie_companies/note");

        mk_mid.open(gendb_dir + "/movie_keyword/movie_id.bin");
        mk_kid.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        cc_mid.open(gendb_dir + "/complete_cast/movie_id.bin");
        cc_subj.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_stat.open(gendb_dir + "/complete_cast/status_id.bin");

        mi_mid.open(gendb_dir + "/movie_info/movie_id.bin");
        mi_itid.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info.load(gendb_dir + "/movie_info/info");

        mix_mid.open(gendb_dir + "/movie_info_idx/movie_id.bin");
        mix_itid.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mix_info.load(gendb_dir + "/movie_info_idx/info");

        idx_mk.open (gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        idx_cc.open (gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        idx_mix.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        idx_mc.open (gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        idx_mi.open (gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
    }

    // ---------------- DIM RESOLUTION ----------------
    int32_t crew_id = -1, verified_id = -1;
    int32_t it1_id = -1, it2_id = -1;
    int16_t us_code = -1;
    std::vector<int32_t> kt_set;
    std::vector<int32_t> kw_set;
    std::vector<uint64_t> cn_bits;     // bitset over company id (1-indexed)
    int32_t cn_max_id = 0;

    {
        GENDB_PHASE("dim_resolution");

        // comp_cast_type
        for (size_t i = 0; i < cct_id.count; ++i) {
            auto s = cct_kind.at(i);
            if (s == std::string_view("crew"))               crew_id     = cct_id[i];
            if (s == std::string_view("complete+verified"))  verified_id = cct_id[i];
        }

        // info_type
        for (size_t i = 0; i < it_id.count; ++i) {
            auto s = it_info.at(i);
            if (s == std::string_view("countries")) it1_id = it_id[i];
            if (s == std::string_view("rating"))    it2_id = it_id[i];
        }

        // keyword set
        const char* kws[4] = {"murder","murder-in-title","blood","violence"};
        size_t      kwl[4] = {6, 15, 5, 8};
        for (size_t i = 0; i < kw_id.count; ++i) {
            auto s = kw_kw.at(i);
            for (int j = 0; j < 4; ++j) {
                if (s.size() == kwl[j] && std::memcmp(s.data(), kws[j], kwl[j]) == 0) {
                    kw_set.push_back(kw_id[i]);
                    break;
                }
            }
        }

        // kind_type set
        for (size_t i = 0; i < kt_id.count; ++i) {
            auto s = kt_kind.at(i);
            if (s == std::string_view("movie") || s == std::string_view("episode"))
                kt_set.push_back(kt_id[i]);
        }

        // us_code in country_code dict
        // dict: entry i (1-indexed in codes) -> dict_off[i-1]..dict_off[i]
        size_t ndict = cn_cc_doff.count - 1;
        for (size_t i = 0; i < ndict; ++i) {
            int64_t a = cn_cc_doff[i], b = cn_cc_doff[i + 1];
            std::string_view s(cn_cc_ddat.data + a, b - a);
            if (s == std::string_view("[us]")) {
                us_code = (int16_t)(i + 1);  // code i+1 references dict entry i
                break;
            }
        }

        // cn_set: company_name rows with country_code != us_code (and code != 0 = NULL? In SQL, != us also excludes NULL conventionally)
        // SQL semantics: country_code != '[us]' — NULLs are NOT included (3VL).
        // So accept only rows where code != 0 (not NULL) AND code != us_code.
        for (size_t i = 0; i < cn_id.count; ++i) {
            if (cn_id[i] > cn_max_id) cn_max_id = cn_id[i];
        }
        cn_bits.assign((cn_max_id / 64) + 2, 0);
        for (size_t i = 0; i < cn_id.count; ++i) {
            int16_t c = cn_cc[i];
            if (c == 0) continue;          // NULL
            if (c == us_code) continue;    // us
            int32_t id = cn_id[i];
            cn_bits[id >> 6] |= (1ULL << (id & 63));
        }

        std::sort(kw_set.begin(), kw_set.end());
        std::sort(kt_set.begin(), kt_set.end());

        std::fprintf(stderr, "[dim] crew=%d verified=%d it1=%d it2=%d us=%d kt=%zu kw=%zu\n",
                     crew_id, verified_id, it1_id, it2_id, (int)us_code,
                     kt_set.size(), kw_set.size());
    }

    // ---------------- MAIN SCAN ----------------
    std::string best_cn_name, best_mix_info, best_t_title;

    // mi.info literal set (small_string_array)
    const char* mi_lits[4]  = {"Sweden","Germany","Swedish","German"};
    size_t      mi_lit_l[4] = {6, 7, 7, 6};

    auto kt_in = [&](int32_t v) {
        for (int32_t x : kt_set) if (x == v) return true;
        return false;
    };
    auto kw_in = [&](int32_t v) {
        for (int32_t x : kw_set) if (x == v) return true;
        return false;
    };

    {
        GENDB_PHASE("main_scan");

        size_t n_title = t_id.count;
        int n_threads = omp_get_max_threads();
        std::vector<std::string> loc_cn(n_threads), loc_mx(n_threads), loc_tt(n_threads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            std::string& local_cn_name  = loc_cn[tid];
            std::string& local_mix_info = loc_mx[tid];
            std::string& local_t_title  = loc_tt[tid];

            #pragma omp for schedule(dynamic, 8192)
            for (size_t r = 0; r < n_title; ++r) {
                int32_t py = t_prod_year[r];
                if (py <= 2005) continue;
                int32_t kid = t_kind_id[r];
                if (!kt_in(kid)) continue;

                int32_t tid_v = t_id[r];

                // ---- Probe mk (most selective) ----
                {
                    int32_t lo = idx_mk[tid_v], hi = idx_mk[tid_v + 1];
                    bool found = false;
                    for (int32_t k = lo; k < hi; ++k) {
                        if (kw_in(mk_kid[k])) { found = true; break; }
                    }
                    if (!found) continue;
                }

                // ---- Probe cc ----
                {
                    int32_t lo = idx_cc[tid_v], hi = idx_cc[tid_v + 1];
                    bool found = false;
                    for (int32_t k = lo; k < hi; ++k) {
                        if (cc_subj[k] == crew_id && cc_stat[k] != verified_id) {
                            found = true; break;
                        }
                    }
                    if (!found) continue;
                }

                // ---- Probe mi_idx, collect min(info) where info > "6.5" and info_type_id == it2_id ----
                std::string_view best_mx;
                bool have_mx = false;
                {
                    int32_t lo = idx_mix[tid_v], hi = idx_mix[tid_v + 1];
                    for (int32_t k = lo; k < hi; ++k) {
                        if (mix_itid[k] != it2_id) continue;
                        auto s = mix_info.at(k);
                        if (s.size() < 4 && s <= std::string_view("6.5")) continue;
                        if (s <= std::string_view("6.5")) continue;
                        if (!have_mx || s < best_mx) { best_mx = s; have_mx = true; }
                    }
                    if (!have_mx) continue;
                }

                // ---- Probe mc (company_id ∈ cn_set, note LIKE filters), collect min(cn.name) ----
                std::string_view best_cn;
                bool have_cn = false;
                {
                    int32_t lo = idx_mc[tid_v], hi = idx_mc[tid_v + 1];
                    for (int32_t k = lo; k < hi; ++k) {
                        // note NOT NULL
                        if (mc_note.is_null(k)) continue;
                        auto note = mc_note.at(k);
                        // NOT LIKE '%(USA)%'
                        if (sv_find(note, "(USA)", 5) != std::string_view::npos) continue;
                        // LIKE '%(200%)%' interpreted as contains "(200" with a ')' after.
                        size_t pos = sv_find(note, "(200", 4);
                        if (pos == std::string_view::npos) continue;
                        // Look for ')' after pos+4
                        bool ok = false;
                        for (size_t p = pos + 4; p < note.size(); ++p) {
                            if (note[p] == ')') { ok = true; break; }
                        }
                        if (!ok) continue;
                        int32_t cid = mc_cid[k];
                        if (cid <= 0 || cid > cn_max_id) continue;
                        if (!((cn_bits[cid >> 6] >> (cid & 63)) & 1ULL)) continue;
                        // candidate -> lookup cn.name (id=cid, row=cid-1)
                        auto nm = cn_name.at((size_t)(cid - 1));
                        if (!have_cn || nm < best_cn) { best_cn = nm; have_cn = true; }
                    }
                    if (!have_cn) continue;
                }

                // ---- Probe mi (info_type_id == it1_id, info ∈ 4 literals) ----
                {
                    int32_t lo = idx_mi[tid_v], hi = idx_mi[tid_v + 1];
                    bool found = false;
                    for (int32_t k = lo; k < hi; ++k) {
                        if (mi_itid[k] != it1_id) continue;
                        auto s = mi_info.at(k);
                        for (int j = 0; j < 4; ++j) {
                            if (s.size() == mi_lit_l[j] &&
                                std::memcmp(s.data(), mi_lits[j], mi_lit_l[j]) == 0) {
                                found = true; break;
                            }
                        }
                        if (found) break;
                    }
                    if (!found) continue;
                }

                // ---- All probes passed; update local mins ----
                std::string cn_s(best_cn);
                std::string mx_s(best_mx);
                auto tt = t_title.at(r);
                std::string tt_s(tt);

                if (local_cn_name.empty() || cn_s < local_cn_name)  local_cn_name  = std::move(cn_s);
                if (local_mix_info.empty() || mx_s < local_mix_info) local_mix_info = std::move(mx_s);
                if (local_t_title.empty() || tt_s < local_t_title)  local_t_title  = std::move(tt_s);
            }
        }

        // Reduce across threads
        for (int i = 0; i < n_threads; ++i) {
            if (!loc_cn[i].empty() && (best_cn_name.empty() || loc_cn[i] < best_cn_name))
                best_cn_name = loc_cn[i];
            if (!loc_mx[i].empty() && (best_mix_info.empty() || loc_mx[i] < best_mix_info))
                best_mix_info = loc_mx[i];
            if (!loc_tt[i].empty() && (best_t_title.empty() || loc_tt[i] < best_t_title))
                best_t_title = loc_tt[i];
        }
    }

    // ---------------- OUTPUT ----------------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q28b.csv";
        std::ofstream out(out_path);
        out << "movie_company,rating,complete_euro_dark_movie\n";
        // CSV with no quoting (matches ground truth file convention)
        out << best_cn_name << "," << best_mix_info << "," << best_t_title << "\n";
    }

    return 0;
}
