// Q21c — IMDB JOB
// SELECT MIN(cn.name), MIN(lt.link), MIN(t.title)
// Driver: movie_link aux CSR by link_type_id (small set matching '%follow%')
// Then per candidate movie: title.production_year range, movie_keyword (sequel),
// movie_info (IN 9 literals), movie_companies (note IS NULL, ct_id, allowed cn).
//
// Storage notes (different from guide):
//   - Indexes live in <gendb_dir>/_idx/, offsets are int32 (not uint64)
//   - Varlen columns use <col>.off (int64 N+1) + <col>.dat
//   - Dim tables are dense 1..N → row = id - 1 (no __id__pos.bin needed)
//   - country_code is dict-encoded int16 (.bin + .dict.off + .dict.dat)

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#define _GNU_SOURCE
#include <string.h>

#include "timing_utils.h"

// Concatenating string-arg parser: supports values that contain spaces
// (e.g. "production companies" was split by the shell into two argv tokens).
// Collects all tokens after --name until next "--flag" or end of argv.
static std::string parse_string_concat(int argc, char* argv[], const char* name, const std::string& dflt) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) != 0) continue;
        std::string out;
        for (int j = i + 1; j < argc; ++j) {
            const char* a = argv[j];
            if (a[0] == '-' && a[1] == '-') break;
            if (!out.empty()) out += ' ';
            out += a;
        }
        if (out.empty()) return dflt;
        return out;
    }
    return dflt;
}

static int64_t parse_int_simple(int argc, char* argv[], const char* name, int64_t dflt) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtoll(argv[i + 1], nullptr, 10);
    }
    return dflt;
}

// ---- Generic mmap helpers ----
struct Mapped {
    const void* p = nullptr;
    size_t len = 0;
    int fd = -1;
    ~Mapped() {
        if (p && len) ::munmap(const_cast<void*>(p), len);
        if (fd >= 0) ::close(fd);
    }
};

static void mmap_file(const std::string& path, Mapped& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "open failed: %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (::fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat failed: %s\n", path.c_str()); std::exit(1); }
    out.len = st.st_size;
    out.fd = fd;
    if (out.len == 0) { out.p = nullptr; return; }
    void* m = ::mmap(nullptr, out.len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { std::fprintf(stderr, "mmap failed: %s\n", path.c_str()); std::exit(1); }
    out.p = m;
    ::madvise(m, out.len, MADV_WILLNEED);
}

template <typename T>
static const T* as(const Mapped& m) { return reinterpret_cast<const T*>(m.p); }

// LIKE %substr% (no other wildcards): strip leading and trailing '%' if present
static bool make_substr(const std::string& pat, std::string& out) {
    out = pat;
    if (!out.empty() && out.front() == '%') out.erase(out.begin());
    if (!out.empty() && out.back() == '%') out.pop_back();
    return true;
}

// memmem search
static bool contains_substr(const char* hay, size_t hay_len, const char* needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (hay_len < needle_len) return false;
    return ::memmem(hay, hay_len, needle, needle_len) != nullptr;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir> [params...]\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];

    // Parse parameters
    int32_t py_lo = (int32_t)parse_int_simple(argc, argv, "--production_year_lower", 1950);
    int32_t py_hi = (int32_t)parse_int_simple(argc, argv, "--production_year_upper", 2010);
    std::string link_pat   = parse_string_concat(argc, argv, "--link_pattern",      "%follow%");
    std::string keyword_eq = parse_string_concat(argc, argv, "--keyword_eq",        "sequel");
    std::string kind_eq    = parse_string_concat(argc, argv, "--kind_eq",           "production companies");
    std::string cc_neq     = parse_string_concat(argc, argv, "--country_code_neq",  "[pl]");
    std::string name_pat1  = parse_string_concat(argc, argv, "--name_pattern",      "%Film%");
    std::string name_pat2  = parse_string_concat(argc, argv, "--name_pattern_2",    "%Warner%");

    std::string link_sub, name_sub1, name_sub2;
    make_substr(link_pat, link_sub);
    make_substr(name_pat1, name_sub1);
    make_substr(name_pat2, name_sub2);

    // ------- mmap all required files -------
    Mapped m_title_id, m_title_py, m_title_off, m_title_dat;
    Mapped m_lt_id, m_lt_off, m_lt_dat;
    Mapped m_cn_id, m_cn_cc, m_cn_cc_doff, m_cn_cc_ddat, m_cn_off, m_cn_dat;
    Mapped m_ct_id, m_ct_kind_off, m_ct_kind_dat;
    Mapped m_kw_id, m_kw_off, m_kw_dat;

    Mapped m_ml_lt_offsets, m_ml_lt_rowids;
    Mapped m_ml_movie_id, m_ml_link_type_id;
    Mapped m_ml_movieoffsets;
    Mapped m_mk_movieoffsets, m_mk_keyword_id;
    Mapped m_mi_movieoffsets, m_mi_info_off, m_mi_info_dat;
    Mapped m_mc_movieoffsets, m_mc_company_id, m_mc_company_type_id, m_mc_note_off;

    {
        GENDB_PHASE("data_loading");
        // title
        mmap_file(gendb_dir + "/title/id.bin",               m_title_id);
        mmap_file(gendb_dir + "/title/production_year.bin",  m_title_py);
        mmap_file(gendb_dir + "/title/title.off",            m_title_off);
        mmap_file(gendb_dir + "/title/title.dat",            m_title_dat);
        // link_type
        mmap_file(gendb_dir + "/link_type/id.bin",           m_lt_id);
        mmap_file(gendb_dir + "/link_type/link.off",         m_lt_off);
        mmap_file(gendb_dir + "/link_type/link.dat",         m_lt_dat);
        // company_name
        mmap_file(gendb_dir + "/company_name/id.bin",                  m_cn_id);
        mmap_file(gendb_dir + "/company_name/country_code.bin",        m_cn_cc);
        mmap_file(gendb_dir + "/company_name/country_code.dict.off",   m_cn_cc_doff);
        mmap_file(gendb_dir + "/company_name/country_code.dict.dat",   m_cn_cc_ddat);
        mmap_file(gendb_dir + "/company_name/name.off",                m_cn_off);
        mmap_file(gendb_dir + "/company_name/name.dat",                m_cn_dat);
        // company_type
        mmap_file(gendb_dir + "/company_type/id.bin",        m_ct_id);
        mmap_file(gendb_dir + "/company_type/kind.off",      m_ct_kind_off);
        mmap_file(gendb_dir + "/company_type/kind.dat",      m_ct_kind_dat);
        // keyword
        mmap_file(gendb_dir + "/keyword/id.bin",             m_kw_id);
        mmap_file(gendb_dir + "/keyword/keyword.off",        m_kw_off);
        mmap_file(gendb_dir + "/keyword/keyword.dat",        m_kw_dat);
        // movie_link
        mmap_file(gendb_dir + "/_idx/movie_link__link_type_id__offsets.bin",  m_ml_lt_offsets);
        mmap_file(gendb_dir + "/_idx/movie_link__link_type_id__rowids.bin",   m_ml_lt_rowids);
        mmap_file(gendb_dir + "/_idx/movie_link__movie_id__offsets.bin",      m_ml_movieoffsets);
        mmap_file(gendb_dir + "/movie_link/movie_id.bin",                     m_ml_movie_id);
        mmap_file(gendb_dir + "/movie_link/link_type_id.bin",                 m_ml_link_type_id);
        // movie_keyword
        mmap_file(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin",   m_mk_movieoffsets);
        mmap_file(gendb_dir + "/movie_keyword/keyword_id.bin",                m_mk_keyword_id);
        // movie_info
        mmap_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin",      m_mi_movieoffsets);
        mmap_file(gendb_dir + "/movie_info/info.off",                          m_mi_info_off);
        mmap_file(gendb_dir + "/movie_info/info.dat",                          m_mi_info_dat);
        // movie_companies
        mmap_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin", m_mc_movieoffsets);
        mmap_file(gendb_dir + "/movie_companies/company_id.bin",              m_mc_company_id);
        mmap_file(gendb_dir + "/movie_companies/company_type_id.bin",         m_mc_company_type_id);
        mmap_file(gendb_dir + "/movie_companies/note.off",                    m_mc_note_off);
    }

    // Convenient typed views
    const int32_t* title_id          = as<int32_t>(m_title_id);
    const int32_t* title_py          = as<int32_t>(m_title_py);
    const int64_t* title_off         = as<int64_t>(m_title_off);
    const char*    title_dat         = as<char>(m_title_dat);
    const size_t   title_rows        = m_title_id.len / 4;

    const int32_t* lt_id             = as<int32_t>(m_lt_id);
    const int64_t* lt_off            = as<int64_t>(m_lt_off);
    const char*    lt_dat            = as<char>(m_lt_dat);
    const size_t   lt_rows           = m_lt_id.len / 4;

    const int32_t* cn_id             = as<int32_t>(m_cn_id);
    const int16_t* cn_cc             = as<int16_t>(m_cn_cc);
    const int64_t* cn_cc_doff        = as<int64_t>(m_cn_cc_doff);
    const char*    cn_cc_ddat        = as<char>(m_cn_cc_ddat);
    const int64_t* cn_off            = as<int64_t>(m_cn_off);
    const char*    cn_dat            = as<char>(m_cn_dat);
    const size_t   cn_rows           = m_cn_id.len / 4;
    const size_t   cn_cc_dict_count  = (m_cn_cc_doff.len / 8) - 1;

    const int32_t* ct_id             = as<int32_t>(m_ct_id);
    const int64_t* ct_kind_off       = as<int64_t>(m_ct_kind_off);
    const char*    ct_kind_dat       = as<char>(m_ct_kind_dat);
    const size_t   ct_rows           = m_ct_id.len / 4;

    const int32_t* kw_id             = as<int32_t>(m_kw_id);
    const int64_t* kw_off            = as<int64_t>(m_kw_off);
    const char*    kw_dat            = as<char>(m_kw_dat);
    const size_t   kw_rows           = m_kw_id.len / 4;

    const int32_t* ml_lt_offsets     = as<int32_t>(m_ml_lt_offsets);
    const int32_t* ml_lt_rowids      = as<int32_t>(m_ml_lt_rowids);
    const int32_t* ml_movie_id       = as<int32_t>(m_ml_movie_id);
    const int32_t* ml_link_type_id   = as<int32_t>(m_ml_link_type_id);
    const int32_t* ml_movieoffsets   = as<int32_t>(m_ml_movieoffsets);

    const int32_t* mk_movieoffsets   = as<int32_t>(m_mk_movieoffsets);
    const int32_t* mk_keyword_id     = as<int32_t>(m_mk_keyword_id);

    const int32_t* mi_movieoffsets   = as<int32_t>(m_mi_movieoffsets);
    const int64_t* mi_info_off       = as<int64_t>(m_mi_info_off);
    const char*    mi_info_dat       = as<char>(m_mi_info_dat);

    const int32_t* mc_movieoffsets   = as<int32_t>(m_mc_movieoffsets);
    const int32_t* mc_company_id     = as<int32_t>(m_mc_company_id);
    const int32_t* mc_company_type_id= as<int32_t>(m_mc_company_type_id);
    const int64_t* mc_note_off       = as<int64_t>(m_mc_note_off);

    // ------- Dimension literal resolution -------
    int32_t keyword_id_target = -1;
    int32_t company_type_id_target = -1;
    std::vector<int32_t> link_type_ids;   // matching link_type.id values
    std::vector<uint8_t> link_type_match(20, 0); // flag by link_type_id (max 18)
    // Pre-fetch lt.link for MIN later: we read on demand via link_type_id -> row = id-1.

    {
        GENDB_PHASE("dim_resolve");

        // keyword: exact match
        for (size_t r = 0; r < kw_rows; ++r) {
            size_t lo = kw_off[r], hi = kw_off[r + 1];
            std::string_view s(kw_dat + lo, hi - lo);
            if (s == keyword_eq) { keyword_id_target = kw_id[r]; break; }
        }
        // company_type: exact match
        for (size_t r = 0; r < ct_rows; ++r) {
            size_t lo = ct_kind_off[r], hi = ct_kind_off[r + 1];
            std::string_view s(ct_kind_dat + lo, hi - lo);
            if (s == kind_eq) { company_type_id_target = ct_id[r]; break; }
        }
        // link_type: LIKE %follow%
        for (size_t r = 0; r < lt_rows; ++r) {
            size_t lo = lt_off[r], hi = lt_off[r + 1];
            if (contains_substr(lt_dat + lo, hi - lo, link_sub.data(), link_sub.size())) {
                int32_t id = lt_id[r];
                link_type_ids.push_back(id);
                if (id >= 0 && (size_t)id < link_type_match.size())
                    link_type_match[(size_t)id] = 1;
            }
        }
    }

    if (keyword_id_target < 0 || company_type_id_target < 0 || link_type_ids.empty()) {
        // No matches possible
        std::string out_path = results_dir + "/results.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (fp) {
            std::fprintf(fp, "company_name,link_type,western_follow_up\n,,\n");
            std::fclose(fp);
        }
        return 0;
    }

    // ------- Filter company_name → allowed_by_id -------
    // pl_dict_code (int16) — code 0 = NULL, real codes 1..K reference dict[code-1]
    int16_t pl_code = -1;
    {
        for (size_t i = 0; i < cn_cc_dict_count; ++i) {
            size_t lo = cn_cc_doff[i], hi = cn_cc_doff[i + 1];
            std::string_view s(cn_cc_ddat + lo, hi - lo);
            if (s == cc_neq) { pl_code = (int16_t)(i + 1); break; }
        }
    }

    // Find max company_id to size the bitmap
    int32_t max_cn_id = 0;
    for (size_t r = 0; r < cn_rows; ++r) if (cn_id[r] > max_cn_id) max_cn_id = cn_id[r];
    std::vector<uint8_t> cn_allowed((size_t)max_cn_id + 2, 0);

    {
        GENDB_PHASE("cn_filter");
        size_t survivors = 0;
        const char* sub1 = name_sub1.data(); size_t l1 = name_sub1.size();
        const char* sub2 = name_sub2.data(); size_t l2 = name_sub2.size();
        for (size_t r = 0; r < cn_rows; ++r) {
            // SQL: col != '[pl]'  — NULL fails this predicate. Reject NULL (code 0) too.
            int16_t code = cn_cc[r];
            if (code == 0) continue;
            if (pl_code > 0 && code == pl_code) continue;
            // name like %Film% OR %Warner%
            size_t lo = cn_off[r], hi = cn_off[r + 1];
            const char* nm = cn_dat + lo;
            size_t nl = hi - lo;
            bool ok = (l1 > 0 && contains_substr(nm, nl, sub1, l1)) ||
                      (l2 > 0 && contains_substr(nm, nl, sub2, l2));
            if (!ok) continue;
            int32_t id = cn_id[r];
            if (id >= 0 && id <= max_cn_id) cn_allowed[(size_t)id] = 1;
            ++survivors;
        }
        (void)survivors;
    }

    // ------- Build movie_info literal hash set -------
    static const char* MI_LITERALS[] = {
        "Sweden","Norway","Germany","Denmark","Swedish","Denish","Norwegian","German","English"
    };
    std::unordered_set<std::string_view> mi_set;
    for (auto* s : MI_LITERALS) mi_set.emplace(std::string_view(s, std::strlen(s)));

    // ------- Drive: movie_link aux CSR by link_type_id → candidate movie_ids -------
    std::vector<int32_t> candidate_movies;
    {
        GENDB_PHASE("build_candidates");
        // ml_lt_offsets size = max_link_type_id + 2 = 20
        for (int32_t v : link_type_ids) {
            int32_t lo = ml_lt_offsets[v];
            int32_t hi = ml_lt_offsets[v + 1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = ml_lt_rowids[k];
                candidate_movies.push_back(ml_movie_id[r]);
            }
        }
        std::sort(candidate_movies.begin(), candidate_movies.end());
        candidate_movies.erase(std::unique(candidate_movies.begin(), candidate_movies.end()),
                               candidate_movies.end());
    }

    // ------- Main scan: per-candidate predicate evaluation + MIN updates -------
    std::string min_cn_name;  bool have_cn = false;
    std::string min_lt_link;  bool have_lt = false;
    std::string min_title;    bool have_tt = false;

    {
        GENDB_PHASE("main_scan");
        for (int32_t v : candidate_movies) {
            if (v < 1 || (size_t)v > title_rows) continue;
            size_t trow = (size_t)v - 1;
            // (a) production_year range
            int32_t py = title_py[trow];
            if (py < py_lo || py > py_hi) continue;

            // (b) movie_keyword semi-join: any mk.keyword_id == keyword_id_target?
            {
                int32_t lo = mk_movieoffsets[v];
                int32_t hi = mk_movieoffsets[v + 1];
                bool found = false;
                for (int32_t r = lo; r < hi; ++r) {
                    if (mk_keyword_id[r] == keyword_id_target) { found = true; break; }
                }
                if (!found) continue;
            }

            // (c) movie_info IN-list semi-join
            {
                int32_t lo = mi_movieoffsets[v];
                int32_t hi = mi_movieoffsets[v + 1];
                bool found = false;
                for (int32_t r = lo; r < hi; ++r) {
                    size_t a = mi_info_off[r], b = mi_info_off[r + 1];
                    std::string_view s(mi_info_dat + a, b - a);
                    if (mi_set.find(s) != mi_set.end()) { found = true; break; }
                }
                if (!found) continue;
            }

            // (d) movie_companies: collect valid mc rows
            // Must have >=1 valid. For MIN(cn.name), update for each valid cn.id.
            std::vector<int32_t> valid_cn_ids;
            {
                int32_t lo = mc_movieoffsets[v];
                int32_t hi = mc_movieoffsets[v + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    // note IS NULL
                    if (mc_note_off[r + 1] != mc_note_off[r]) continue;
                    if (mc_company_type_id[r] != company_type_id_target) continue;
                    int32_t cid = mc_company_id[r];
                    if (cid < 0 || cid > max_cn_id) continue;
                    if (!cn_allowed[(size_t)cid]) continue;
                    valid_cn_ids.push_back(cid);
                }
                if (valid_cn_ids.empty()) continue;
            }

            // (e) movie_link: enumerate ml of this movie with link_type_id in link_type_ids
            // (must have >=1; by construction it does, but we re-test for MIN(lt.link))
            std::vector<int32_t> valid_lt_ids;
            {
                int32_t lo = ml_movieoffsets[v];
                int32_t hi = ml_movieoffsets[v + 1];
                for (int32_t r = lo; r < hi; ++r) {
                    int32_t lti = ml_link_type_id[r];
                    if (lti >= 0 && (size_t)lti < link_type_match.size() && link_type_match[(size_t)lti]) {
                        valid_lt_ids.push_back(lti);
                    }
                }
                if (valid_lt_ids.empty()) continue;
            }

            // All predicates passed → contribute to MINs

            // MIN(t.title)
            {
                size_t a = title_off[trow], b = title_off[trow + 1];
                std::string_view tv(title_dat + a, b - a);
                if (!have_tt || tv < std::string_view(min_title)) {
                    min_title.assign(tv);
                    have_tt = true;
                }
            }

            // MIN(cn.name) over valid cn ids
            for (int32_t cid : valid_cn_ids) {
                size_t crow = (size_t)cid - 1;
                size_t a = cn_off[crow], b = cn_off[crow + 1];
                std::string_view nv(cn_dat + a, b - a);
                if (!have_cn || nv < std::string_view(min_cn_name)) {
                    min_cn_name.assign(nv);
                    have_cn = true;
                }
            }

            // MIN(lt.link) over valid lt ids (dedup not necessary; just compare)
            for (int32_t lti : valid_lt_ids) {
                size_t lrow = (size_t)lti - 1;
                size_t a = lt_off[lrow], b = lt_off[lrow + 1];
                std::string_view lv(lt_dat + a, b - a);
                if (!have_lt || lv < std::string_view(min_lt_link)) {
                    min_lt_link.assign(lv);
                    have_lt = true;
                }
            }
        }
    }

    // ------- Output -------
    {
        GENDB_PHASE("output");
        // ensure results_dir exists (best-effort)
        ::mkdir(results_dir.c_str(), 0755);
        std::string out_path = results_dir + "/Q21c.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(fp, "company_name,link_type,western_follow_up\n");
        auto csv_write = [&](const std::string& s, bool have) {
            if (!have) { /* empty */ return; }
            // If contains comma, quote and escape internal quotes (none expected here)
            bool need_quote = false;
            for (char c : s) if (c == ',' || c == '"' || c == '\n') { need_quote = true; break; }
            if (!need_quote) {
                std::fwrite(s.data(), 1, s.size(), fp);
            } else {
                std::fputc('"', fp);
                for (char c : s) {
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
            }
        };
        csv_write(min_cn_name, have_cn);
        std::fputc(',', fp);
        csv_write(min_lt_link, have_lt);
        std::fputc(',', fp);
        csv_write(min_title, have_tt);
        std::fputc('\n', fp);
        std::fclose(fp);
    }

    return 0;
}
