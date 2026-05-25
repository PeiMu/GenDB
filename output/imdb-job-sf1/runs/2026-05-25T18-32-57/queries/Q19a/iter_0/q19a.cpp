// Q19a — IMDB JOB query
// MIN(n.name), MIN(t.title) over a 9-table join. Driver: title (year filter).
// Probe mc, mi, ci via primary CSR on movie_id. Semi-joins via dense-id row math.
//
// Storage layout (per storage_design.json):
//  * Varlen columns: <col>.dat (raw bytes) + <col>.off (int64[N+1] byte offsets)
//  * Dict columns:   <col>.bin (int8 or int16 codes; 0 = NULL) +
//                    <col>.dict.dat + <col>.dict.off (int64[K+1])
//  * Nullable int32: sentinel = INT32_MIN
//  * Offsets-only/CSR index: int32[max_parent_id + 2], in dir _idx/
//  * Tables with id_is_dense_1_to_N: id = row + 1 → no pos table needed

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>

#include "cli_params.h"
#include "timing_utils.h"

using std::string;
using std::string_view;

// ---------- mmap ----------
static const void* map_file(const string& path, size_t* out_sz) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path.c_str(), strerror(errno));
        std::exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "stat %s failed\n", path.c_str());
        std::exit(1);
    }
    *out_sz = (size_t)st.st_size;
    void* p = nullptr;
    if (st.st_size > 0) {
        p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mmap %s failed: %s\n", path.c_str(), strerror(errno));
            std::exit(1);
        }
        madvise(p, st.st_size, MADV_WILLNEED);
    }
    close(fd);
    return p;
}

// ---------- LIKE pattern matcher ----------
struct LikePat {
    std::vector<string> parts;
    bool anchor_start;
    bool anchor_end;
};

static LikePat parse_like(const string& pat) {
    LikePat r;
    r.anchor_start = !pat.empty() && pat[0] != '%';
    r.anchor_end   = !pat.empty() && pat.back() != '%';
    size_t i = 0;
    while (i < pat.size()) {
        if (pat[i] == '%') { i++; continue; }
        size_t j = i;
        while (j < pat.size() && pat[j] != '%') j++;
        r.parts.emplace_back(pat.substr(i, j - i));
        i = j;
    }
    return r;
}

static inline bool like_match(const char* s, size_t len, const LikePat& p) {
    if (p.parts.empty()) return true;
    size_t lo = 0, hi = len;
    size_t pi = 0, pj = p.parts.size();

    if (p.anchor_start) {
        const string& part = p.parts[pi];
        if (hi - lo < part.size()) return false;
        if (memcmp(s + lo, part.data(), part.size()) != 0) return false;
        lo += part.size();
        pi++;
    }
    if (p.anchor_end) {
        if (pi < pj) {
            const string& part = p.parts[pj - 1];
            if (hi - lo < part.size()) return false;
            if (memcmp(s + hi - part.size(), part.data(), part.size()) != 0) return false;
            hi -= part.size();
            pj--;
        } else {
            if (lo != hi) return false;
        }
    }
    for (size_t k = pi; k < pj; k++) {
        const string& part = p.parts[k];
        if (hi - lo < part.size()) return false;
        const void* found = memmem(s + lo, hi - lo, part.data(), part.size());
        if (!found) return false;
        lo = (const char*)found - s + part.size();
    }
    return true;
}

// ---------- CSV escape ----------
static void csv_emit(FILE* f, string_view v) {
    bool need_quote = false;
    for (char c : v) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        fwrite(v.data(), 1, v.size(), f);
        return;
    }
    fputc('"', f);
    for (char c : v) {
        if (c == '"') fputc('"', f);
        fputc(c, f);
    }
    fputc('"', f);
}

// ---------- dictionary lookup ----------
// dict files: <col>.dict.off (int64[K+1]) + <col>.dict.dat (raw bytes).
// Returns code (1-based; code 0 = NULL) or -1 if not found.
static int32_t dict_lookup(const int64_t* doff, size_t doff_count,
                           const char* ddat, const string& target) {
    // doff_count = K+1 entries → K dict entries
    size_t K = doff_count - 1;
    for (size_t k = 0; k < K; k++) {
        size_t s = (size_t)doff[k], e = (size_t)doff[k+1];
        if (e - s == target.size() && memcmp(ddat + s, target.data(), target.size()) == 0) {
            return (int32_t)(k + 1);  // code is 1-based
        }
    }
    return -1;
}

// ---------- main ----------
int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [params...]\n", argv[0]);
        return 1;
    }
    string storage = argv[1];
    string results = argv[2];

    // ---------- parameters ----------
    int64_t year_lo   = gendb::parse_int_arg(argc, argv, "--production_year_lower", 2005);
    int64_t year_hi   = gendb::parse_int_arg(argc, argv, "--production_year_upper", 2009);
    string  role_eq   = gendb::parse_string_arg(argc, argv, "--role_eq", "actress");
    string  name_pat_s   = gendb::parse_string_arg(argc, argv, "--name_pattern", "%Ang%");
    string  gender_eq_s  = gendb::parse_string_arg(argc, argv, "--gender_eq", "f");
    string  info_pat1_s  = gendb::parse_string_arg(argc, argv, "--info_pattern",   "Japan:%200%");
    string  info_pat2_s  = gendb::parse_string_arg(argc, argv, "--info_pattern_2", "USA:%200%");
    string  info_eq      = gendb::parse_string_arg(argc, argv, "--info_eq", "release dates");
    string  note_pat1_s  = gendb::parse_string_arg(argc, argv, "--note_pattern",   "%(USA)%");
    string  note_pat2_s  = gendb::parse_string_arg(argc, argv, "--note_pattern_2", "%(worldwide)%");
    string  cc_eq        = gendb::parse_string_arg(argc, argv, "--country_code_eq", "[us]");

    LikePat name_pat  = parse_like(name_pat_s);
    LikePat info_pat1 = parse_like(info_pat1_s);
    LikePat info_pat2 = parse_like(info_pat2_s);
    LikePat note_pat1 = parse_like(note_pat1_s);
    LikePat note_pat2 = parse_like(note_pat2_s);
    int32_t ylo = (int32_t)year_lo, yhi = (int32_t)year_hi;

    // ---------- mmap data ----------
    size_t sz;
    // name (dense ids 1..N)
    const int8_t*   name_gender;             size_t name_N;
    const int64_t*  name_name_off;
    const char*     name_name_data;
    const int64_t*  name_gender_doff;        size_t name_gender_dcnt;
    const char*     name_gender_ddat;
    // aka_name
    const int32_t*  aka_person_id;           size_t aka_N;
    // company_name (dense ids 1..N)
    const int16_t*  cn_country_code_codes;   size_t cn_N;
    const int64_t*  cn_cc_doff;              size_t cn_cc_dcnt;
    const char*     cn_cc_ddat;
    // title (dense ids 1..N)
    const int32_t*  title_year;              size_t title_N;
    const int64_t*  title_title_off;
    const char*     title_title_data;
    // movie_companies
    const int32_t*  mc_company_id;           size_t mc_N;
    const int64_t*  mc_note_off;
    const char*     mc_note_data;
    // movie_info
    const int32_t*  mi_info_type_id;         size_t mi_N;
    const int64_t*  mi_info_off;
    const char*     mi_info_data;
    // cast_info
    const int32_t*  ci_role_id;              size_t ci_N;
    const int64_t*  ci_note_off;
    const char*     ci_note_data;
    const int32_t*  ci_person_id;
    const int32_t*  ci_person_role_id;
    // role_type / info_type
    const int32_t*  rt_id;                   size_t rt_N;
    const int64_t*  rt_role_off;
    const char*     rt_role_data;
    const int32_t*  it_id;                   size_t it_N;
    const int64_t*  it_info_off;
    const char*     it_info_data;
    // CSR offsets (int32, size = max_parent_id + 2)
    const int32_t*  mc_csr;                  size_t mc_csr_N;
    const int32_t*  mi_csr;                  size_t mi_csr_N;
    const int32_t*  ci_csr;                  size_t ci_csr_N;
    const int32_t*  aka_csr;                 size_t aka_csr_N;

    {
        GENDB_PHASE("data_loading");
        // name
        (void) map_file(storage + "/name/id.bin", &sz); name_N = sz/4;
        name_gender      = (const int8_t*)  map_file(storage + "/name/gender.bin", &sz);
        name_name_off    = (const int64_t*) map_file(storage + "/name/name.off", &sz);
        name_name_data   = (const char*)    map_file(storage + "/name/name.dat", &sz);
        name_gender_doff = (const int64_t*) map_file(storage + "/name/gender.dict.off", &sz);
        name_gender_dcnt = sz / 8;
        name_gender_ddat = (const char*)    map_file(storage + "/name/gender.dict.dat", &sz);

        aka_person_id    = (const int32_t*) map_file(storage + "/aka_name/person_id.bin", &sz); aka_N = sz/4;

        cn_country_code_codes = (const int16_t*) map_file(storage + "/company_name/country_code.bin", &sz); cn_N = sz/2;
        cn_cc_doff       = (const int64_t*) map_file(storage + "/company_name/country_code.dict.off", &sz);
        cn_cc_dcnt       = sz / 8;
        cn_cc_ddat       = (const char*)    map_file(storage + "/company_name/country_code.dict.dat", &sz);

        // title (dense id) — we iterate rows directly. row r has id = r+1
        title_year       = (const int32_t*) map_file(storage + "/title/production_year.bin", &sz); title_N = sz/4;
        title_title_off  = (const int64_t*) map_file(storage + "/title/title.off", &sz);
        title_title_data = (const char*)    map_file(storage + "/title/title.dat", &sz);

        mc_company_id    = (const int32_t*) map_file(storage + "/movie_companies/company_id.bin", &sz); mc_N = sz/4;
        mc_note_off      = (const int64_t*) map_file(storage + "/movie_companies/note.off", &sz);
        mc_note_data     = (const char*)    map_file(storage + "/movie_companies/note.dat", &sz);

        mi_info_type_id  = (const int32_t*) map_file(storage + "/movie_info/info_type_id.bin", &sz); mi_N = sz/4;
        mi_info_off      = (const int64_t*) map_file(storage + "/movie_info/info.off", &sz);
        mi_info_data     = (const char*)    map_file(storage + "/movie_info/info.dat", &sz);

        ci_role_id       = (const int32_t*) map_file(storage + "/cast_info/role_id.bin", &sz); ci_N = sz/4;
        ci_note_off      = (const int64_t*) map_file(storage + "/cast_info/note.off", &sz);
        ci_note_data     = (const char*)    map_file(storage + "/cast_info/note.dat", &sz);
        ci_person_id     = (const int32_t*) map_file(storage + "/cast_info/person_id.bin", &sz);
        ci_person_role_id= (const int32_t*) map_file(storage + "/cast_info/person_role_id.bin", &sz);

        rt_id            = (const int32_t*) map_file(storage + "/role_type/id.bin", &sz); rt_N = sz/4;
        rt_role_off      = (const int64_t*) map_file(storage + "/role_type/role.off", &sz);
        rt_role_data     = (const char*)    map_file(storage + "/role_type/role.dat", &sz);

        it_id            = (const int32_t*) map_file(storage + "/info_type/id.bin", &sz); it_N = sz/4;
        it_info_off      = (const int64_t*) map_file(storage + "/info_type/info.off", &sz);
        it_info_data     = (const char*)    map_file(storage + "/info_type/info.dat", &sz);

        mc_csr  = (const int32_t*) map_file(storage + "/_idx/movie_companies__movie_id__offsets.bin", &sz); mc_csr_N = sz/4;
        mi_csr  = (const int32_t*) map_file(storage + "/_idx/movie_info__movie_id__offsets.bin",     &sz); mi_csr_N = sz/4;
        ci_csr  = (const int32_t*) map_file(storage + "/_idx/cast_info__movie_id__offsets.bin",      &sz); ci_csr_N = sz/4;
        aka_csr = (const int32_t*) map_file(storage + "/_idx/aka_name__person_id__offsets.bin",      &sz); aka_csr_N = sz/4;
    }

    // ---------- resolve dimension literals ----------
    int32_t actress_role_id = -1;
    for (size_t r = 0; r < rt_N; r++) {
        size_t s = (size_t)rt_role_off[r], e = (size_t)rt_role_off[r+1];
        string_view sv(rt_role_data + s, e - s);
        if (sv == role_eq) { actress_role_id = rt_id[r]; break; }
    }
    int32_t release_info_id = -1;
    for (size_t r = 0; r < it_N; r++) {
        size_t s = (size_t)it_info_off[r], e = (size_t)it_info_off[r+1];
        string_view sv(it_info_data + s, e - s);
        if (sv == info_eq) { release_info_id = it_id[r]; break; }
    }
    // Gender dict code for 'f'
    int32_t gender_code = dict_lookup(name_gender_doff, name_gender_dcnt,
                                       name_gender_ddat, gender_eq_s);
    // Country code dict code for '[us]'
    int32_t cc_code = dict_lookup(cn_cc_doff, cn_cc_dcnt, cn_cc_ddat, cc_eq);

    if (actress_role_id < 0 || release_info_id < 0 || gender_code < 0 || cc_code < 0) {
        // No matching dim → emit empty result.
        string out_path = results + "/Q19a.csv";
        FILE* f = fopen(out_path.c_str(), "w");
        if (f) { fprintf(f, "voicing_actress,voiced_movie\n,\n"); fclose(f); }
        return 0;
    }

    // ---------- name prefilter: gender + LIKE %Ang% → bitset over name.id space ----------
    // name has dense ids 1..N, so id == row+1.
    std::vector<uint8_t> name_bits(name_N + 2, 0);  // index by id
    int8_t gb = (int8_t)gender_code;
    {
        GENDB_PHASE("name_filter");
        #pragma omp parallel for schedule(static)
        for (size_t r = 0; r < name_N; r++) {
            if (name_gender[r] != gb) continue;
            size_t s = (size_t)name_name_off[r], e = (size_t)name_name_off[r+1];
            if (e <= s) continue;
            if (!like_match(name_name_data + s, e - s, name_pat)) continue;
            // dense id = r + 1
            name_bits[r + 1] = 1;
        }
    }

    // ---------- aka_name semi-join bitset (over name.id space) ----------
    std::vector<uint8_t> aka_bits(name_N + 2, 0);
    {
        GENDB_PHASE("aka_filter");
        #pragma omp parallel for schedule(static)
        for (size_t r = 0; r < aka_N; r++) {
            int32_t pid = aka_person_id[r];
            if (pid >= 1 && (size_t)pid <= name_N && name_bits[pid]) {
                aka_bits[pid] = 1;
            }
        }
    }

    // ---------- company_name [us] bitset (over cn.id space) ----------
    // company_name has dense ids 1..cn_N.
    std::vector<uint8_t> us_bits(cn_N + 2, 0);
    int16_t cc_code16 = (int16_t)cc_code;
    {
        GENDB_PHASE("company_filter");
        #pragma omp parallel for schedule(static)
        for (size_t r = 0; r < cn_N; r++) {
            if (cn_country_code_codes[r] == cc_code16) {
                us_bits[r + 1] = 1;  // dense id = row + 1
            }
        }
    }

    // ---------- title prefilter — collect surviving title rows (= id - 1) ----------
    int nthr = omp_get_max_threads();
    std::vector<std::vector<int32_t>> tl_rows(nthr);
    {
        GENDB_PHASE("title_filter");
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& vec = tl_rows[tid];
            vec.reserve(32768);
            #pragma omp for schedule(static) nowait
            for (size_t r = 0; r < title_N; r++) {
                int32_t y = title_year[r];
                if (y >= ylo && y <= yhi) {
                    vec.push_back((int32_t)r);
                }
            }
        }
    }
    std::vector<int32_t> trows;
    {
        size_t total = 0;
        for (auto& v : tl_rows) total += v.size();
        trows.reserve(total);
        for (int t = 0; t < nthr; t++) {
            trows.insert(trows.end(), tl_rows[t].begin(), tl_rows[t].end());
        }
    }

    // CSR has max_parent_id + 2 entries. Max valid v: max_parent_id = csr_N - 2.
    int32_t mc_max_v = (int32_t)mc_csr_N - 2;
    int32_t mi_max_v = (int32_t)mi_csr_N - 2;
    int32_t ci_max_v = (int32_t)ci_csr_N - 2;

    // Voice note set
    static const char V_voice[]  = "(voice)";                  // 7
    static const char V_uncred[] = "(voice) (uncredited)";     // 20
    static const char V_eng[]    = "(voice: English version)"; // 24
    static const char V_jap[]    = "(voice: Japanese version)";// 25

    struct LocalMin {
        string_view min_name;
        string_view min_title;
        bool has_name = false;
        bool has_title = false;
    };
    std::vector<LocalMin> tl_min(nthr);

    {
        GENDB_PHASE("main_scan");
        const size_t NT = trows.size();
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            LocalMin& lm = tl_min[tid];

            #pragma omp for schedule(dynamic, 256) nowait
            for (size_t i = 0; i < NT; i++) {
                int32_t trow = trows[i];
                int32_t v = trow + 1;  // title.id (dense)

                // ---- mc filter: any matching mc row for this title ----
                if (v > mc_max_v) continue;
                int32_t mc_lo = mc_csr[v];
                int32_t mc_hi = mc_csr[v + 1];
                bool mc_ok = false;
                for (int32_t r = mc_lo; r < mc_hi; r++) {
                    int64_t ns = mc_note_off[r], ne = mc_note_off[r+1];
                    if (ne <= ns) continue;  // mc.note IS NULL
                    int32_t cid = mc_company_id[r];
                    if (cid < 1 || (size_t)cid > cn_N || !us_bits[cid]) continue;
                    const char* np = mc_note_data + ns;
                    size_t nl = (size_t)(ne - ns);
                    if (!like_match(np, nl, note_pat1) && !like_match(np, nl, note_pat2)) continue;
                    mc_ok = true;
                    break;
                }
                if (!mc_ok) continue;

                // ---- mi filter ----
                if (v > mi_max_v) continue;
                int32_t mi_lo = mi_csr[v];
                int32_t mi_hi = mi_csr[v + 1];
                bool mi_ok = false;
                for (int32_t r = mi_lo; r < mi_hi; r++) {
                    if (mi_info_type_id[r] != release_info_id) continue;
                    int64_t is_ = mi_info_off[r], ie = mi_info_off[r+1];
                    if (ie <= is_) continue;
                    const char* ip = mi_info_data + is_;
                    size_t il = (size_t)(ie - is_);
                    if (!like_match(ip, il, info_pat1) && !like_match(ip, il, info_pat2)) continue;
                    mi_ok = true;
                    break;
                }
                if (!mi_ok) continue;

                // ---- ci filter ----
                if (v > ci_max_v) continue;
                int32_t ci_lo = ci_csr[v];
                int32_t ci_hi = ci_csr[v + 1];

                bool any_ci = false;
                for (int32_t r = ci_lo; r < ci_hi; r++) {
                    if (ci_role_id[r] != actress_role_id) continue;

                    int32_t prid = ci_person_role_id[r];
                    // char_name ids are dense 1..3140339 → existence test is range check
                    if (prid < 1) continue;  // NULL = INT32_MIN; also rejects -1

                    int32_t pid = ci_person_id[r];
                    if (pid < 1 || (size_t)pid > name_N) continue;
                    if (!name_bits[pid]) continue;
                    if (!aka_bits[pid]) continue;

                    int64_t ns = ci_note_off[r], ne = ci_note_off[r+1];
                    if (ne <= ns) continue;
                    size_t nl = (size_t)(ne - ns);
                    const char* np = ci_note_data + ns;
                    bool note_ok = false;
                    switch (nl) {
                        case 7:  note_ok = (memcmp(np, V_voice,  7)  == 0); break;
                        case 20: note_ok = (memcmp(np, V_uncred, 20) == 0); break;
                        case 24: note_ok = (memcmp(np, V_eng,    24) == 0); break;
                        case 25: note_ok = (memcmp(np, V_jap,    25) == 0); break;
                    }
                    if (!note_ok) continue;

                    // surviving tuple — fetch n.name (dense: row = pid - 1)
                    int32_t nrow = pid - 1;
                    int64_t na_s = name_name_off[nrow], na_e = name_name_off[nrow + 1];
                    string_view nv(name_name_data + na_s, (size_t)(na_e - na_s));
                    if (!lm.has_name || nv < lm.min_name) {
                        lm.has_name = true;
                        lm.min_name = nv;
                    }
                    any_ci = true;
                }
                if (!any_ci) continue;

                // update t.title min (title is dense; row = trow)
                int64_t ts = title_title_off[trow], te = title_title_off[trow + 1];
                string_view ttv(title_title_data + ts, (size_t)(te - ts));
                if (!lm.has_title || ttv < lm.min_title) {
                    lm.has_title = true;
                    lm.min_title = ttv;
                }
            }
        }
    }

    // ---------- merge ----------
    string_view g_name, g_title;
    bool has_name = false, has_title = false;
    for (auto& lm : tl_min) {
        if (lm.has_name && (!has_name || lm.min_name < g_name)) {
            g_name = lm.min_name; has_name = true;
        }
        if (lm.has_title && (!has_title || lm.min_title < g_title)) {
            g_title = lm.min_title; has_title = true;
        }
    }

    // ---------- output ----------
    {
        GENDB_PHASE("output");
        string out_path = results + "/Q19a.csv";
        FILE* f = fopen(out_path.c_str(), "w");
        if (!f) {
            fprintf(stderr, "fopen %s: %s\n", out_path.c_str(), strerror(errno));
            return 1;
        }
        fprintf(f, "voicing_actress,voiced_movie\n");
        if (has_name || has_title) {
            if (has_name) csv_emit(f, g_name);
            fputc(',', f);
            if (has_title) csv_emit(f, g_title);
            fputc('\n', f);
        } else {
            fprintf(f, ",\n");
        }
        fclose(f);
    }

    (void)aka_csr; (void)aka_csr_N;  // not needed for this query
    return 0;
}
