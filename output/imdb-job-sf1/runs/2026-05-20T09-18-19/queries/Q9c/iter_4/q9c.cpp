// Q9c — voicing actresses with female + %An% + US movies + voice notes
// Strategy:
//  1. Resolve constants.
//  2. Build cn_us_bitset.
//  3. Build mc_movie_has_us_bitset (parallel, thread-local + merge).
//  4. Build ci_voice_bitset (parallel, chunked static).
//  5. Build name_pid_bitset (gender=f AND name LIKE '%An%').
//  6. Driver pid list = name_pid AND aka_name presence.
//  7. Probe per pid: walk CSR cast_info__person_id, gated by ci_voice and mc_us.
//  8. Aggregate per-thread MINs, merge.

#include <omp.h>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"

using namespace gendb;
using std::string;
using std::string_view;

static inline bool is_voice_note(string_view s) {
    return s == "(voice)" ||
           s == "(voice: Japanese version)" ||
           s == "(voice) (uncredited)" ||
           s == "(voice: English version)";
}

static inline bool has_An(const char* p, size_t len) {
    if (len < 2) return false;
    const char* end = p + len - 1;
    for (const char* c = p; c < end; ++c) {
        if (c[0] == 'A' && c[1] == 'n') return true;
    }
    return false;
}

struct LMin {
    const char* p = nullptr;
    size_t len = 0;
    bool set = false;
    inline void update(const char* np, size_t nl) {
        if (!set) { p = np; len = nl; set = true; return; }
        size_t m = len < nl ? len : nl;
        int c = std::memcmp(np, p, m);
        if (c < 0 || (c == 0 && nl < len)) { p = np; len = nl; }
    }
    inline void merge(const LMin& o) {
        if (o.set) update(o.p, o.len);
    }
};

static string csv_escape(const char* p, size_t len) {
    bool need_quote = false;
    for (size_t i = 0; i < len; ++i) {
        char c = p[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) return string(p, len);
    string out;
    out.reserve(len + 2);
    out.push_back('"');
    for (size_t i = 0; i < len; ++i) {
        char c = p[i];
        if (c == '"') { out.push_back('"'); out.push_back('"'); }
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    string store = argv[1];
    string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---- mmap all columns / indexes ----
    MmapColumn<int32_t> ci_role_id, ci_movie_id, ci_person_role_id;
    MmapColumn<int64_t> ci_note_off;
    MmapColumn<char>    ci_note_dat;
    MmapColumn<int32_t> mc_movie_id, mc_company_id;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<int64_t> name_name_off, an_name_off, chn_name_off, t_title_off;
    MmapColumn<int64_t> rt_role_off, g_off, cc_off;
    MmapColumn<char>    name_name_dat, an_name_dat, chn_name_dat, t_title_dat;
    MmapColumn<char>    rt_role_dat, g_dat, cc_dat;
    MmapColumn<int32_t> cipid_off, cipid_row, anpid_off;

    {
        GENDB_PHASE("data_loading");
        ci_role_id.open(store + "/cast_info/role_id.bin");
        ci_movie_id.open(store + "/cast_info/movie_id.bin");
        ci_person_role_id.open(store + "/cast_info/person_role_id.bin");
        ci_note_off.open(store + "/cast_info/note.off");
        ci_note_dat.open(store + "/cast_info/note.dat");

        mc_movie_id.open(store + "/movie_companies/movie_id.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");

        cn_country_code.open(store + "/company_name/country_code.bin");
        cc_off.open(store + "/company_name/country_code.dict.off");
        cc_dat.open(store + "/company_name/country_code.dict.dat");

        name_gender.open(store + "/name/gender.bin");
        g_off.open(store + "/name/gender.dict.off");
        g_dat.open(store + "/name/gender.dict.dat");
        name_name_off.open(store + "/name/name.off");
        name_name_dat.open(store + "/name/name.dat");

        an_name_off.open(store + "/aka_name/name.off");
        an_name_dat.open(store + "/aka_name/name.dat");

        chn_name_off.open(store + "/char_name/name.off");
        chn_name_dat.open(store + "/char_name/name.dat");

        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        rt_role_off.open(store + "/role_type/role.off");
        rt_role_dat.open(store + "/role_type/role.dat");

        cipid_off.open(store + "/_idx/cast_info__person_id__offsets.bin");
        cipid_row.open(store + "/_idx/cast_info__person_id__rowids.bin");
        anpid_off.open(store + "/_idx/aka_name__person_id__offsets.bin");
    }

    // ---- Resolve constants ----
    int32_t rt_id_actress = 0;
    int16_t us_code = 0;
    int8_t  f_code = 0;
    {
        GENDB_PHASE("resolve_constants");
        for (size_t i = 0; i + 1 < rt_role_off.count; ++i) {
            string_view v(rt_role_dat.data + rt_role_off[i], rt_role_off[i+1] - rt_role_off[i]);
            if (v == "actress") { rt_id_actress = (int32_t)(i + 1); break; }
        }
        for (size_t i = 0; i + 1 < cc_off.count; ++i) {
            string_view v(cc_dat.data + cc_off[i], cc_off[i+1] - cc_off[i]);
            if (v == "[us]") { us_code = (int16_t)(i + 1); break; }
        }
        for (size_t i = 0; i + 1 < g_off.count; ++i) {
            string_view v(g_dat.data + g_off[i], g_off[i+1] - g_off[i]);
            if (v == "f") { f_code = (int8_t)(i + 1); break; }
        }
    }

    const int NTHR = 12;
    omp_set_num_threads(NTHR);

    // ---- Build cn_us_bitset ----
    const size_t N_cn = cn_country_code.count;
    std::vector<uint64_t> cn_us_bs((N_cn + 63) / 64, 0);
    {
        GENDB_PHASE("build_cn_us_bitset");
        for (size_t i = 0; i < N_cn; ++i) {
            if (cn_country_code[i] == us_code) cn_us_bs[i >> 6] |= (1ULL << (i & 63));
        }
    }

    // ---- Build mc_movie_has_us_bitset ----
    const size_t N_title = t_title_off.count - 1;  // dense PK titles
    const size_t W_title = (N_title + 63) / 64;
    std::vector<uint64_t> mc_us_bs(W_title, 0);
    {
        GENDB_PHASE("build_mc_movie_has_us_bitset");
        const size_t N_mc = mc_movie_id.count;
        std::vector<std::vector<uint64_t>> local(NTHR, std::vector<uint64_t>(W_title, 0));
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& bs = local[tid];
            #pragma omp for schedule(static)
            for (size_t i = 0; i < N_mc; ++i) {
                int32_t cid = mc_company_id[i];
                if (cid < 1) continue;
                uint32_t idx = (uint32_t)(cid - 1);
                if (!(cn_us_bs[idx >> 6] & (1ULL << (idx & 63)))) continue;
                int32_t mid = mc_movie_id[i];
                if (mid < 1) continue;
                uint32_t midx = (uint32_t)(mid - 1);
                bs[midx >> 6] |= (1ULL << (midx & 63));
            }
        }
        // merge
        #pragma omp parallel for schedule(static)
        for (size_t k = 0; k < W_title; ++k) {
            uint64_t acc = 0;
            for (int t = 0; t < NTHR; ++t) acc |= local[t][k];
            mc_us_bs[k] = acc;
        }
    }

    // ---- Build ci_voice_bitset ----
    const size_t N_ci = ci_role_id.count;
    const size_t W_ci = (N_ci + 63) / 64;
    std::vector<uint64_t> ci_voice_bs(W_ci, 0);
    {
        GENDB_PHASE("build_ci_voice_bitset");
        // Chunk size 4096 = 64-word aligned => no race on word boundaries.
        #pragma omp parallel for schedule(static, 4096)
        for (size_t r = 0; r < N_ci; ++r) {
            if (ci_role_id[r] != rt_id_actress) continue;
            if (ci_person_role_id[r] == INT32_MIN) continue;
            int64_t lo = ci_note_off[r];
            int64_t hi = ci_note_off[r + 1];
            if (hi - lo < 7) continue;  // shortest voice note "(voice)" = 7 chars
            string_view nv(ci_note_dat.data + lo, (size_t)(hi - lo));
            if (!is_voice_note(nv)) continue;
            ci_voice_bs[r >> 6] |= (1ULL << (r & 63));
        }
    }

    // ---- Build name_pid_bitset ----
    const size_t N_name = name_gender.count;
    const size_t W_name = (N_name + 63) / 64;
    std::vector<uint64_t> name_pid_bs(W_name, 0);
    {
        GENDB_PHASE("build_name_pid_bitset");
        #pragma omp parallel for schedule(static, 4096)
        for (size_t i = 0; i < N_name; ++i) {
            if (name_gender[i] != f_code) continue;
            int64_t lo = name_name_off[i];
            int64_t hi = name_name_off[i + 1];
            const char* p = name_name_dat.data + lo;
            size_t len = (size_t)(hi - lo);
            if (!has_An(p, len)) continue;
            name_pid_bs[i >> 6] |= (1ULL << (i & 63));
        }
    }

    // ---- Build driver pid list (intersect with aka_name presence) ----
    std::vector<int32_t> driver_pids;
    {
        GENDB_PHASE("build_driver_list");
        driver_pids.reserve(65536);
        for (size_t i = 0; i < N_name; ++i) {
            if (!(name_pid_bs[i >> 6] & (1ULL << (i & 63)))) continue;
            int32_t pid = (int32_t)(i + 1);
            // aka_name presence: anpid_off[pid+1] > anpid_off[pid]
            if (anpid_off[pid + 1] > anpid_off[pid]) {
                driver_pids.push_back(pid);
            }
        }
    }

    // ---- Probe per pid ----
    std::vector<LMin> tl_an(NTHR), tl_chn(NTHR), tl_n(NTHR), tl_t(NTHR);
    {
        GENDB_PHASE("probe_per_pid");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            LMin lan, lchn, ln, lt;
            const size_t D = driver_pids.size();

            #pragma omp for schedule(dynamic, 256)
            for (size_t di = 0; di < D; ++di) {
                int32_t pid = driver_pids[di];
                int32_t lo = cipid_off[pid];
                int32_t hi = cipid_off[pid + 1];
                bool survived = false;
                for (int32_t k = lo; k < hi; ++k) {
                    int32_t r = cipid_row[k];
                    if (!(ci_voice_bs[(size_t)r >> 6] & (1ULL << ((size_t)r & 63)))) continue;
                    int32_t mid = ci_movie_id[r];
                    if (mid < 1) continue;
                    uint32_t midx = (uint32_t)(mid - 1);
                    if (!(mc_us_bs[midx >> 6] & (1ULL << (midx & 63)))) continue;
                    int32_t chn_id = ci_person_role_id[r];
                    if (chn_id < 1) continue;
                    // chn name
                    int64_t clo = chn_name_off[chn_id - 1];
                    int64_t chi = chn_name_off[chn_id];
                    lchn.update(chn_name_dat.data + clo, (size_t)(chi - clo));
                    // t title
                    int64_t tlo = t_title_off[midx];
                    int64_t thi = t_title_off[midx + 1];
                    lt.update(t_title_dat.data + tlo, (size_t)(thi - tlo));
                    survived = true;
                }
                if (survived) {
                    // n.name[pid-1]
                    size_t pi = (size_t)pid - 1;
                    int64_t nlo = name_name_off[pi];
                    int64_t nhi = name_name_off[pi + 1];
                    ln.update(name_name_dat.data + nlo, (size_t)(nhi - nlo));
                    // aka_name range
                    int32_t alo = anpid_off[pid];
                    int32_t ahi = anpid_off[pid + 1];
                    for (int32_t ar = alo; ar < ahi; ++ar) {
                        int64_t aolo = an_name_off[ar];
                        int64_t aohi = an_name_off[ar + 1];
                        lan.update(an_name_dat.data + aolo, (size_t)(aohi - aolo));
                    }
                }
            }
            tl_an[tid] = lan;
            tl_chn[tid] = lchn;
            tl_n[tid] = ln;
            tl_t[tid] = lt;
        }
    }

    // Merge thread-local mins
    LMin g_an, g_chn, g_n, g_t;
    for (int t = 0; t < NTHR; ++t) {
        g_an.merge(tl_an[t]);
        g_chn.merge(tl_chn[t]);
        g_n.merge(tl_n[t]);
        g_t.merge(tl_t[t]);
    }

    // ---- Write output ----
    {
        GENDB_PHASE("output");
        std::ofstream out(results_dir + "/Q9c.csv");
        out << "alternative_name,voiced_character_name,voicing_actress,american_movie\n";
        auto col = [&](const LMin& m) {
            if (m.set) return csv_escape(m.p, m.len);
            return string();
        };
        out << col(g_an) << "," << col(g_chn) << "," << col(g_n) << "," << col(g_t) << "\n";
    }

    return 0;
}
