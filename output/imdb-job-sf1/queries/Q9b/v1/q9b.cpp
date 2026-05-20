// Q9b: voiced character / alternative name / voicing actress / american movie
// Person-driven join pipeline using cast_info CSR on person_id.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

// ------------- file helpers -------------
static std::string read_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(("open " + path).c_str()); exit(1); }
    struct stat st; fstat(fd, &st);
    std::string out;
    out.resize(st.st_size);
    ssize_t got = 0;
    char* p = out.data();
    while (got < st.st_size) {
        ssize_t r = read(fd, p + got, st.st_size - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    return out;
}

// ------------- substring search -------------
static inline bool contains(const char* hay, size_t hlen, const char* needle, size_t nlen) {
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    return memmem(hay, hlen, needle, nlen) != nullptr;
}

// ------------- main -------------
int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    // ---- mmap / load all columns and indexes ----
    MmapColumn<int8_t>   n_gender_col;
    MmapColumn<int64_t>  n_name_off_col;
    MmapColumn<char>     n_name_dat_col;
    MmapColumn<int8_t>   cn_cc_col;
    MmapColumn<int32_t>  title_py_col;
    MmapColumn<int64_t>  title_title_off_col;
    MmapColumn<char>     title_title_dat_col;

    MmapColumn<int32_t>  ci_role_id_col;
    MmapColumn<int32_t>  ci_movie_id_col;
    MmapColumn<int32_t>  ci_prole_col;
    MmapColumn<int64_t>  ci_note_off_col;
    MmapColumn<char>     ci_note_dat_col;

    MmapColumn<int32_t>  mc_movie_id_col;  // not used directly (range from offsets)
    MmapColumn<int32_t>  mc_company_id_col;
    MmapColumn<int64_t>  mc_note_off_col;
    MmapColumn<char>     mc_note_dat_col;

    MmapColumn<int32_t>  aka_pid_col;  // not used directly
    MmapColumn<int64_t>  aka_name_off_col;
    MmapColumn<char>     aka_name_dat_col;

    MmapColumn<int64_t>  chn_name_off_col;
    MmapColumn<char>     chn_name_dat_col;

    MmapColumn<int32_t>  cipid_off_col;
    MmapColumn<int32_t>  cipid_row_col;
    MmapColumn<int32_t>  mc_movie_off_col;
    MmapColumn<int32_t>  aka_pid_off_col;

    int16_t us_code = 0;
    int8_t  f_code  = 0;
    int32_t rt_id_actress = 0;

    {
        GENDB_PHASE("data_loading");

        // Dict resolution: role_type role -> rt_id_actress
        {
            auto rt_off = read_file(store + "/role_type/role.off");
            auto rt_dat = read_file(store + "/role_type/role.dat");
            const int64_t* off = reinterpret_cast<const int64_t*>(rt_off.data());
            size_t n = rt_off.size() / sizeof(int64_t);
            for (size_t i = 0; i + 1 < n; ++i) {
                std::string_view s(rt_dat.data() + off[i], off[i+1] - off[i]);
                if (s == "actress") { rt_id_actress = (int32_t)(i + 1); break; }
            }
        }
        // cn.country_code dict -> us_code
        {
            auto cc_off = read_file(store + "/company_name/country_code.dict.off");
            auto cc_dat = read_file(store + "/company_name/country_code.dict.dat");
            const int64_t* off = reinterpret_cast<const int64_t*>(cc_off.data());
            size_t n = cc_off.size() / sizeof(int64_t);
            for (size_t i = 0; i + 1 < n; ++i) {
                std::string_view s(cc_dat.data() + off[i], off[i+1] - off[i]);
                if (s == "[us]") { us_code = (int16_t)(i + 1); break; }
            }
        }
        // name.gender dict -> f_code
        {
            auto g_off = read_file(store + "/name/gender.dict.off");
            auto g_dat = read_file(store + "/name/gender.dict.dat");
            const int64_t* off = reinterpret_cast<const int64_t*>(g_off.data());
            size_t n = g_off.size() / sizeof(int64_t);
            for (size_t i = 0; i + 1 < n; ++i) {
                std::string_view s(g_dat.data() + off[i], off[i+1] - off[i]);
                if (s == "f") { f_code = (int8_t)(i + 1); break; }
            }
        }

        n_gender_col.open(store + "/name/gender.bin");
        n_name_off_col.open(store + "/name/name.off");
        n_name_dat_col.open(store + "/name/name.dat");

        // cn country_code stored as int16 dict; the guide example used int16 but file is 469994 = 2*234997
        // open as int16
        cn_cc_col.open(store + "/company_name/country_code.bin");
        // Note: cn_cc_col declared as int8_t above is wrong. We'll re-open below using a typed alias.

        title_py_col.open(store + "/title/production_year.bin");
        title_title_off_col.open(store + "/title/title.off");
        title_title_dat_col.open(store + "/title/title.dat");

        ci_role_id_col.open(store + "/cast_info/role_id.bin");
        ci_movie_id_col.open(store + "/cast_info/movie_id.bin");
        ci_prole_col.open(store + "/cast_info/person_role_id.bin");
        ci_note_off_col.open(store + "/cast_info/note.off");
        ci_note_dat_col.open(store + "/cast_info/note.dat");

        mc_company_id_col.open(store + "/movie_companies/company_id.bin");
        mc_note_off_col.open(store + "/movie_companies/note.off");
        mc_note_dat_col.open(store + "/movie_companies/note.dat");

        aka_name_off_col.open(store + "/aka_name/name.off");
        aka_name_dat_col.open(store + "/aka_name/name.dat");

        chn_name_off_col.open(store + "/char_name/name.off");
        chn_name_dat_col.open(store + "/char_name/name.dat");

        cipid_off_col.open(store + "/_idx/cast_info__person_id__offsets.bin");
        cipid_row_col.open(store + "/_idx/cast_info__person_id__rowids.bin");
        mc_movie_off_col.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        aka_pid_off_col.open(store + "/_idx/aka_name__person_id__offsets.bin");
    }

    // Re-mmap cn country_code as int16
    MmapColumn<int16_t> cn_cc_i16(store + "/company_name/country_code.bin");

    if (rt_id_actress == 0 || us_code == 0 || f_code == 0) {
        fprintf(stderr, "dict resolution failed: rt=%d us=%d f=%d\n",
                rt_id_actress, (int)us_code, (int)f_code);
        return 1;
    }

    const int8_t*  n_gender = n_gender_col.data;
    const int64_t* n_name_off = n_name_off_col.data;
    const char*    n_name_dat = n_name_dat_col.data;
    size_t name_rows = n_gender_col.count;

    const int16_t* cn_cc = cn_cc_i16.data;
    const int32_t* title_py = title_py_col.data;
    const int64_t* title_title_off = title_title_off_col.data;
    const char*    title_title_dat = title_title_dat_col.data;

    const int32_t* ci_role_id = ci_role_id_col.data;
    const int32_t* ci_movie_id = ci_movie_id_col.data;
    const int32_t* ci_prole = ci_prole_col.data;
    const int64_t* ci_note_off = ci_note_off_col.data;
    const char*    ci_note_dat = ci_note_dat_col.data;

    const int32_t* mc_company_id = mc_company_id_col.data;
    const int64_t* mc_note_off = mc_note_off_col.data;
    const char*    mc_note_dat = mc_note_dat_col.data;

    const int64_t* aka_name_off = aka_name_off_col.data;
    const char*    aka_name_dat = aka_name_dat_col.data;

    const int64_t* chn_name_off = chn_name_off_col.data;
    const char*    chn_name_dat = chn_name_dat_col.data;

    const int32_t* cipid_off = cipid_off_col.data;
    const int32_t* cipid_row = cipid_row_col.data;
    const int32_t* mc_movie_off = mc_movie_off_col.data;
    const int32_t* aka_pid_off = aka_pid_off_col.data;

    // ---- Step 1: parallel name scan to collect pids ----
    std::vector<int32_t> pids;
    {
        GENDB_PHASE("scan_name_filter");
        unsigned nthr = std::max(1u, std::thread::hardware_concurrency());
        if (nthr > 12) nthr = 12;
        std::vector<std::vector<int32_t>> local(nthr);
        std::vector<std::thread> ths;
        size_t chunk = (name_rows + nthr - 1) / nthr;
        for (unsigned t = 0; t < nthr; ++t) {
            size_t b = t * chunk;
            size_t e = std::min<size_t>(name_rows, b + chunk);
            ths.emplace_back([&, t, b, e]() {
                auto& v = local[t];
                for (size_t i = b; i < e; ++i) {
                    if (n_gender[i] != f_code) continue;
                    int64_t off = n_name_off[i];
                    int64_t len = n_name_off[i+1] - off;
                    if (len < 5) continue;
                    if (memmem(n_name_dat + off, len, "Angel", 5)) {
                        v.push_back((int32_t)(i + 1));
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
        size_t total = 0;
        for (auto& v : local) total += v.size();
        pids.reserve(total);
        for (auto& v : local) pids.insert(pids.end(), v.begin(), v.end());
        std::sort(pids.begin(), pids.end());
    }
    fprintf(stderr, "[Q9b] candidate pids: %zu\n", pids.size());

    // ---- Step 2-8: per-pid pipeline (parallel) ----
    struct Result {
        std::string an_min;
        std::string chn_min;
        std::string n_min;
        std::string t_min;
        bool has = false;
    };

    unsigned nthr = std::max(1u, std::thread::hardware_concurrency());
    if (nthr > 12) nthr = 12;
    if ((size_t)nthr > pids.size()) nthr = std::max<unsigned>(1, (unsigned)pids.size());
    std::vector<Result> locals(nthr);
    std::vector<std::thread> threads;

    {
        GENDB_PHASE("main_scan");
        size_t chunk = (pids.size() + nthr - 1) / nthr;
        for (unsigned t = 0; t < nthr; ++t) {
            size_t b = t * chunk;
            size_t e = std::min<size_t>(pids.size(), b + chunk);
            threads.emplace_back([&, t, b, e]() {
                Result& R = locals[t];
                for (size_t idx = b; idx < e; ++idx) {
                    int32_t pid = pids[idx];

                    // aka_name range
                    int32_t a_lo = aka_pid_off[pid];
                    int32_t a_hi = aka_pid_off[pid + 1];
                    if (a_lo >= a_hi) continue;  // need non-empty

                    int32_t lo = cipid_off[pid];
                    int32_t hi = cipid_off[pid + 1];

                    for (int32_t k = lo; k < hi; ++k) {
                        int32_t r = cipid_row[k];

                        if (ci_role_id[r] != rt_id_actress) continue;

                        int64_t no = ci_note_off[r];
                        int64_t nl = ci_note_off[r + 1] - no;
                        if (nl != 7) continue;
                        if (memcmp(ci_note_dat + no, "(voice)", 7) != 0) continue;

                        int32_t prole = ci_prole[r];
                        if (prole == INT32_MIN) continue;

                        int32_t mv = ci_movie_id[r];
                        int32_t py = title_py[mv - 1];
                        if (py < 2007 || py > 2010) continue;

                        // movie_companies range
                        int32_t m_lo = mc_movie_off[mv];
                        int32_t m_hi = mc_movie_off[mv + 1];
                        bool mc_ok = false;
                        for (int32_t mk = m_lo; mk < m_hi; ++mk) {
                            int32_t cid = mc_company_id[mk];
                            if (cid <= 0) continue;
                            if (cn_cc[cid - 1] != us_code) continue;

                            int64_t mno = mc_note_off[mk];
                            int64_t mnl = mc_note_off[mk + 1] - mno;
                            if (mnl <= 0) continue;
                            const char* nd = mc_note_dat + mno;
                            if (!contains(nd, mnl, "(200", 4)) continue;
                            // need ')' after the 3 digits — pattern '%(200%)%'
                            // We check this by looking for "(200" then any char then ")" eventually.
                            // The plan says note LIKE '%(200%)%' — SQL LIKE: '(200' anywhere then ')' anywhere after.
                            // Verify there's a ')' somewhere after the matched "(200".
                            {
                                const char* hit = (const char*)memmem(nd, mnl, "(200", 4);
                                if (!hit) continue;
                                int64_t rest_off = (hit + 4) - nd;
                                if (rest_off >= mnl) continue;
                                if (!memchr(nd + rest_off, ')', mnl - rest_off)) continue;
                            }
                            if (!contains(nd, mnl, "(USA)", 5) &&
                                !contains(nd, mnl, "(worldwide)", 11)) continue;
                            mc_ok = true;
                            break;
                        }
                        if (!mc_ok) continue;

                        // chn lookup
                        int32_t chn_id = prole;
                        if (chn_id <= 0) continue;
                        int64_t co = chn_name_off[chn_id - 1];
                        int64_t cl = chn_name_off[chn_id] - co;
                        std::string chn_s(chn_name_dat + co, (size_t)cl);

                        // aka min
                        std::string an_s;
                        bool an_has = false;
                        for (int32_t ai = a_lo; ai < a_hi; ++ai) {
                            int64_t ao = aka_name_off[ai];
                            int64_t al = aka_name_off[ai + 1] - ao;
                            std::string_view sv(aka_name_dat + ao, (size_t)al);
                            if (!an_has || sv < std::string_view(an_s)) {
                                an_s.assign(sv);
                                an_has = true;
                            }
                        }
                        if (!an_has) continue;

                        // n.name
                        int64_t no_ = n_name_off[pid - 1];
                        int64_t nl_ = n_name_off[pid] - no_;
                        std::string n_s(n_name_dat + no_, (size_t)nl_);

                        // t.title
                        int64_t to = title_title_off[mv - 1];
                        int64_t tl = title_title_off[mv] - to;
                        std::string t_s(title_title_dat + to, (size_t)tl);

                        if (!R.has) {
                            R.an_min = an_s; R.chn_min = chn_s; R.n_min = n_s; R.t_min = t_s;
                            R.has = true;
                        } else {
                            if (an_s < R.an_min)  R.an_min = an_s;
                            if (chn_s < R.chn_min) R.chn_min = chn_s;
                            if (n_s < R.n_min)   R.n_min = n_s;
                            if (t_s < R.t_min)   R.t_min = t_s;
                        }
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // ---- reduce ----
    Result final;
    for (auto& L : locals) {
        if (!L.has) continue;
        if (!final.has) { final = L; continue; }
        if (L.an_min  < final.an_min)  final.an_min  = L.an_min;
        if (L.chn_min < final.chn_min) final.chn_min = L.chn_min;
        if (L.n_min   < final.n_min)   final.n_min   = L.n_min;
        if (L.t_min   < final.t_min)   final.t_min   = L.t_min;
    }

    // ---- output CSV ----
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q9b.csv";
        FILE* fp = fopen(out_path.c_str(), "w");
        if (!fp) { perror("fopen"); return 1; }
        fprintf(fp, "alternative_name,voiced_character,voicing_actress,american_movie\n");
        if (final.has) {
            // CSV-escape: wrap in quotes if contains comma or quote
            auto csv = [](const std::string& s) -> std::string {
                bool need = s.find(',') != std::string::npos
                         || s.find('"') != std::string::npos
                         || s.find('\n') != std::string::npos;
                if (!need) return s;
                std::string o; o.reserve(s.size() + 2);
                o.push_back('"');
                for (char c : s) {
                    if (c == '"') o.push_back('"');
                    o.push_back(c);
                }
                o.push_back('"');
                return o;
            };
            fprintf(fp, "%s,%s,%s,%s\n",
                    csv(final.an_min).c_str(),
                    csv(final.chn_min).c_str(),
                    csv(final.n_min).c_str(),
                    csv(final.t_min).c_str());
        }
        fclose(fp);
    }

    return 0;
}
