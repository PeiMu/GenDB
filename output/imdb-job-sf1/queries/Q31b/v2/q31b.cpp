// Q31b — IMDB JOB
// MIN(mi.info), MIN(mi_idx.info), MIN(n.name), MIN(t.title)
// Driver: title (~2.5M rows). Year > 2000 AND (title LIKE %Freddy%/%Jason%/Saw%)
// is very selective. Then per-candidate: semi-join mk, mc; project-mins on
// mi, mi_idx, ci+name.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "timing_utils.h"
#include "cli_params.h"

// ----------------- mmap helpers -----------------
struct Mapping {
    const void* ptr = nullptr;
    size_t size = 0;
};
static Mapping mmap_file(const std::string& path) {
    Mapping m;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", path.c_str());
        std::exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::fprintf(stderr, "stat failed: %s\n", path.c_str());
        std::exit(1);
    }
    m.size = (size_t)st.st_size;
    if (m.size > 0) {
        void* p = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "mmap failed: %s\n", path.c_str());
            std::exit(1);
        }
        madvise(p, m.size, MADV_RANDOM);
        m.ptr = p;
    }
    ::close(fd);
    return m;
}
template<typename T>
static const T* as(const Mapping& m) { return (const T*)m.ptr; }

// ----------------- Pattern classification -----------------
struct Pattern {
    enum Kind { PREFIX, SUFFIX, SUBSTR, EXACT } kind;
    std::string lit;
};
static Pattern parse_like(const std::string& s) {
    Pattern p;
    bool lead = !s.empty() && s.front() == '%';
    bool tail = !s.empty() && s.back()  == '%';
    size_t a = lead ? 1 : 0;
    size_t b = tail ? s.size() - 1 : s.size();
    p.lit = s.substr(a, b - a);
    if (lead && tail) p.kind = Pattern::SUBSTR;
    else if (lead)    p.kind = Pattern::SUFFIX;
    else if (tail)    p.kind = Pattern::PREFIX;
    else              p.kind = Pattern::EXACT;
    return p;
}
static inline bool match_pattern(const Pattern& p, const char* data, size_t len) {
    const size_t L = p.lit.size();
    if (L == 0) return p.kind != Pattern::EXACT || len == 0;
    switch (p.kind) {
        case Pattern::PREFIX: return len >= L && std::memcmp(data, p.lit.data(), L) == 0;
        case Pattern::SUFFIX: return len >= L && std::memcmp(data + len - L, p.lit.data(), L) == 0;
        case Pattern::SUBSTR: return memmem(data, len, p.lit.data(), L) != nullptr;
        case Pattern::EXACT:  return len == L && std::memcmp(data, p.lit.data(), L) == 0;
    }
    return false;
}

static inline void update_min(std::string& cur, bool& set, std::string_view v) {
    if (!set || v < std::string_view(cur)) {
        cur.assign(v.data(), v.size());
        set = true;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir> [--params...]\n", argv[0]);
        return 1;
    }
    gendb::init_date_tables();
    const std::string gendb_dir = argv[1];
    const std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    // Params
    int64_t production_year_lower = gendb::parse_int_arg(argc, argv, "--production_year_lower", 2000);
    std::string gender_eq      = gendb::parse_string_arg(argc, argv, "--gender_eq", "m");
    std::string title_pattern  = gendb::parse_string_arg(argc, argv, "--title_pattern",   "Saw%");
    std::string title_pattern_2= gendb::parse_string_arg(argc, argv, "--title_pattern_2", "%Freddy%");
    std::string title_pattern_3= gendb::parse_string_arg(argc, argv, "--title_pattern_3", "%Jason%");
    std::string note_pattern   = gendb::parse_string_arg(argc, argv, "--note_pattern",    "%(Blu-ray)%");
    std::string info_eq        = gendb::parse_string_arg(argc, argv, "--info_eq",         "votes");
    std::string info_eq_2      = gendb::parse_string_arg(argc, argv, "--info_eq_2",       "genres");
    std::string name_pattern   = gendb::parse_string_arg(argc, argv, "--name_pattern",    "Lionsgate%");

    Pattern pat_t1 = parse_like(title_pattern);
    Pattern pat_t2 = parse_like(title_pattern_2);
    Pattern pat_t3 = parse_like(title_pattern_3);
    Pattern pat_note = parse_like(note_pattern);
    Pattern pat_cn = parse_like(name_pattern);

    GENDB_PHASE("total");

    // ---------- Data loading ----------
    Mapping m_title_id, m_title_year, m_title_off, m_title_data;
    Mapping m_ci_note_off, m_ci_note_data, m_ci_person;
    Mapping m_cn_id, m_cn_name_off, m_cn_name_data;
    Mapping m_it_id, m_it_info_off, m_it_info_data;
    Mapping m_kw_id, m_kw_off, m_kw_data;
    Mapping m_mc_cid, m_mc_note_off, m_mc_note_data;
    Mapping m_mi_typeid, m_mi_info_off, m_mi_info_data;
    Mapping m_mii_typeid, m_mii_info_off, m_mii_info_data;
    Mapping m_mk_kwid;
    Mapping m_name_gender, m_name_off, m_name_data;
    Mapping m_name_gender_dict_off, m_name_gender_dict_dat;

    Mapping m_idx_mc, m_idx_mi, m_idx_mii, m_idx_mk, m_idx_ci;

    {
        GENDB_PHASE("data_loading");
        // title
        m_title_id   = mmap_file(gendb_dir + "/title/id.bin");
        m_title_year = mmap_file(gendb_dir + "/title/production_year.bin");
        m_title_off  = mmap_file(gendb_dir + "/title/title.off");
        m_title_data = mmap_file(gendb_dir + "/title/title.dat");
        // cast_info
        m_ci_note_off  = mmap_file(gendb_dir + "/cast_info/note.off");
        m_ci_note_data = mmap_file(gendb_dir + "/cast_info/note.dat");
        m_ci_person    = mmap_file(gendb_dir + "/cast_info/person_id.bin");
        // company_name
        m_cn_id        = mmap_file(gendb_dir + "/company_name/id.bin");
        m_cn_name_off  = mmap_file(gendb_dir + "/company_name/name.off");
        m_cn_name_data = mmap_file(gendb_dir + "/company_name/name.dat");
        // info_type
        m_it_id        = mmap_file(gendb_dir + "/info_type/id.bin");
        m_it_info_off  = mmap_file(gendb_dir + "/info_type/info.off");
        m_it_info_data = mmap_file(gendb_dir + "/info_type/info.dat");
        // keyword
        m_kw_id   = mmap_file(gendb_dir + "/keyword/id.bin");
        m_kw_off  = mmap_file(gendb_dir + "/keyword/keyword.off");
        m_kw_data = mmap_file(gendb_dir + "/keyword/keyword.dat");
        // movie_companies
        m_mc_cid       = mmap_file(gendb_dir + "/movie_companies/company_id.bin");
        m_mc_note_off  = mmap_file(gendb_dir + "/movie_companies/note.off");
        m_mc_note_data = mmap_file(gendb_dir + "/movie_companies/note.dat");
        // movie_info
        m_mi_typeid    = mmap_file(gendb_dir + "/movie_info/info_type_id.bin");
        m_mi_info_off  = mmap_file(gendb_dir + "/movie_info/info.off");
        m_mi_info_data = mmap_file(gendb_dir + "/movie_info/info.dat");
        // movie_info_idx
        m_mii_typeid    = mmap_file(gendb_dir + "/movie_info_idx/info_type_id.bin");
        m_mii_info_off  = mmap_file(gendb_dir + "/movie_info_idx/info.off");
        m_mii_info_data = mmap_file(gendb_dir + "/movie_info_idx/info.dat");
        // movie_keyword
        m_mk_kwid = mmap_file(gendb_dir + "/movie_keyword/keyword_id.bin");
        // name
        m_name_gender = mmap_file(gendb_dir + "/name/gender.bin");
        m_name_off    = mmap_file(gendb_dir + "/name/name.off");
        m_name_data   = mmap_file(gendb_dir + "/name/name.dat");
        m_name_gender_dict_off = mmap_file(gendb_dir + "/name/gender.dict.off");
        m_name_gender_dict_dat = mmap_file(gendb_dir + "/name/gender.dict.dat");
        // indexes (CSR offsets are uint32_t; max_movie_id+2 entries)
        m_idx_mc   = mmap_file(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        m_idx_mi   = mmap_file(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        m_idx_mii  = mmap_file(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        m_idx_mk   = mmap_file(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        m_idx_ci   = mmap_file(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
    }

    const int32_t* title_id    = as<int32_t>(m_title_id);
    const int32_t* title_year  = as<int32_t>(m_title_year);
    const uint64_t* title_off  = as<uint64_t>(m_title_off);
    const char*    title_data  = as<char>(m_title_data);
    const size_t   N_title = m_title_id.size / sizeof(int32_t);

    const uint64_t* ci_note_off  = as<uint64_t>(m_ci_note_off);
    const char*    ci_note_data  = as<char>(m_ci_note_data);
    const int32_t* ci_person     = as<int32_t>(m_ci_person);

    const int32_t* cn_id       = as<int32_t>(m_cn_id);
    const uint64_t* cn_name_off = as<uint64_t>(m_cn_name_off);
    const char*    cn_name_data = as<char>(m_cn_name_data);
    const size_t   N_cn = m_cn_id.size / sizeof(int32_t);

    const int32_t* it_id        = as<int32_t>(m_it_id);
    const uint64_t* it_info_off = as<uint64_t>(m_it_info_off);
    const char*    it_info_data = as<char>(m_it_info_data);
    const size_t   N_it = m_it_id.size / sizeof(int32_t);

    const int32_t* kw_id   = as<int32_t>(m_kw_id);
    const uint64_t* kw_off = as<uint64_t>(m_kw_off);
    const char*    kw_data = as<char>(m_kw_data);
    const size_t   N_kw = m_kw_id.size / sizeof(int32_t);

    const int32_t* mc_cid    = as<int32_t>(m_mc_cid);
    const uint64_t* mc_note_off = as<uint64_t>(m_mc_note_off);
    const char*    mc_note_data = as<char>(m_mc_note_data);

    const int32_t*  mi_typeid    = as<int32_t>(m_mi_typeid);
    const uint64_t* mi_info_off  = as<uint64_t>(m_mi_info_off);
    const char*     mi_info_data = as<char>(m_mi_info_data);

    const int32_t*  mii_typeid    = as<int32_t>(m_mii_typeid);
    const uint64_t* mii_info_off  = as<uint64_t>(m_mii_info_off);
    const char*     mii_info_data = as<char>(m_mii_info_data);

    const int32_t* mk_kwid = as<int32_t>(m_mk_kwid);

    const uint8_t*  name_gender = as<uint8_t>(m_name_gender);
    const uint64_t* name_off    = as<uint64_t>(m_name_off);
    const char*     name_data   = as<char>(m_name_data);
    const size_t    N_name      = m_name_gender.size / sizeof(uint8_t);

    const uint64_t* gender_dict_off = as<uint64_t>(m_name_gender_dict_off);
    const char*     gender_dict_dat = as<char>(m_name_gender_dict_dat);
    const size_t    N_gender_dict   = (m_name_gender_dict_off.size / sizeof(uint64_t)) - 1;

    const uint32_t* off_mc  = as<uint32_t>(m_idx_mc);
    const uint32_t* off_mi  = as<uint32_t>(m_idx_mi);
    const uint32_t* off_mii = as<uint32_t>(m_idx_mii);
    const uint32_t* off_mk  = as<uint32_t>(m_idx_mk);
    const uint32_t* off_ci  = as<uint32_t>(m_idx_ci);

    // ---------- Resolve dimension literals ----------
    int32_t it1_id = -1; // 'genres'
    int32_t it2_id = -1; // 'votes'
    std::unordered_set<int32_t> k_id_set;
    std::unordered_set<int32_t> cn_id_set;
    // Resolve gender byte code: scan dict for gender_eq literal; storage uses code+1
    // (with 0 reserved as NULL). Fall back to 0 (no match) if not found.
    uint8_t gender_byte = 0;

    {
        GENDB_PHASE("resolve_dims");
        std::string_view info_eq_sv(info_eq);
        std::string_view info_eq2_sv(info_eq_2);
        for (size_t r = 0; r < N_it; ++r) {
            std::string_view s(it_info_data + it_info_off[r], it_info_off[r+1] - it_info_off[r]);
            if (it1_id < 0 && s == info_eq2_sv) it1_id = it_id[r];
            if (it2_id < 0 && s == info_eq_sv)  it2_id = it_id[r];
            if (it1_id >= 0 && it2_id >= 0) break;
        }

        static const char* kw_list[] = {"murder","violence","blood","gore","death","female-nudity","hospital"};
        std::unordered_set<std::string_view> kw_target;
        for (auto s : kw_list) kw_target.emplace(s);
        for (size_t r = 0; r < N_kw; ++r) {
            std::string_view s(kw_data + kw_off[r], kw_off[r+1] - kw_off[r]);
            if (kw_target.count(s)) k_id_set.insert(kw_id[r]);
        }

        for (size_t r = 0; r < N_cn; ++r) {
            const char* d = cn_name_data + cn_name_off[r];
            size_t len = cn_name_off[r+1] - cn_name_off[r];
            if (match_pattern(pat_cn, d, len)) cn_id_set.insert(cn_id[r]);
        }

        // Gender dict resolution. Encoding observed: storage byte = dict_index + 1,
        // with 0 reserved as NULL sentinel.
        std::string_view gender_target(gender_eq);
        for (size_t r = 0; r < N_gender_dict; ++r) {
            std::string_view s(gender_dict_dat + gender_dict_off[r],
                               gender_dict_off[r+1] - gender_dict_off[r]);
            if (s == gender_target) {
                gender_byte = (uint8_t)(r + 1);
                break;
            }
        }
    }

    // ci.note set
    static const char* ci_note_list[] = {"(writer)","(head writer)","(written by)","(story)","(story editor)"};
    std::unordered_set<std::string_view> ci_note_set;
    for (auto s : ci_note_list) ci_note_set.emplace(s);

    // mi.info set
    static const char* mi_info_list[] = {"Horror","Thriller"};
    std::unordered_set<std::string_view> mi_info_set;
    for (auto s : mi_info_list) mi_info_set.emplace(s);

    // ---------- Main scan: drive on title ----------
    struct LocalMin {
        std::string mi_min, mii_min, name_min, title_min;
        bool has_mi=false, has_mii=false, has_name=false, has_title=false;
    };

    int nthreads = std::max(1, (int)std::thread::hardware_concurrency());
    if (nthreads > 12) nthreads = 12;
    omp_set_num_threads(nthreads);
    std::vector<LocalMin> locals(nthreads);

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel for schedule(dynamic, 16384)
        for (size_t r = 0; r < N_title; ++r) {
            int tid = omp_get_thread_num();
            int32_t year = title_year[r];
            if (year <= (int32_t)production_year_lower) continue;
            const char* td = title_data + title_off[r];
            size_t tlen = title_off[r+1] - title_off[r];
            if (!(match_pattern(pat_t1, td, tlen) ||
                  match_pattern(pat_t2, td, tlen) ||
                  match_pattern(pat_t3, td, tlen))) continue;
            int32_t v = title_id[r];
            if (v <= 0) continue;
            uint64_t vv = (uint64_t)v;

            // mk semi-join
            {
                uint32_t lo = off_mk[vv], hi = off_mk[vv+1];
                bool found = false;
                for (uint32_t i = lo; i < hi; ++i) {
                    if (k_id_set.count(mk_kwid[i])) { found = true; break; }
                }
                if (!found) continue;
            }

            // mc semi-join: company_id in cn_id_set AND note LIKE '%(Blu-ray)%'
            {
                uint32_t lo = off_mc[vv], hi = off_mc[vv+1];
                bool found = false;
                for (uint32_t i = lo; i < hi; ++i) {
                    if (!cn_id_set.count(mc_cid[i])) continue;
                    const char* nd = mc_note_data + mc_note_off[i];
                    size_t nl = mc_note_off[i+1] - mc_note_off[i];
                    if (match_pattern(pat_note, nd, nl)) { found = true; break; }
                }
                if (!found) continue;
            }

            // mi: project MIN(info) where info_type_id == it1 AND info ∈ mi_info_set
            std::string_view mi_local;
            bool mi_local_set = false;
            {
                uint32_t lo = off_mi[vv], hi = off_mi[vv+1];
                for (uint32_t i = lo; i < hi; ++i) {
                    if (mi_typeid[i] != it1_id) continue;
                    std::string_view s(mi_info_data + mi_info_off[i],
                                       mi_info_off[i+1] - mi_info_off[i]);
                    if (!mi_info_set.count(s)) continue;
                    if (!mi_local_set || s < mi_local) { mi_local = s; mi_local_set = true; }
                }
                if (!mi_local_set) continue;
            }

            // mi_idx: project MIN(info) where info_type_id == it2
            std::string_view mii_local;
            bool mii_local_set = false;
            {
                uint32_t lo = off_mii[vv], hi = off_mii[vv+1];
                for (uint32_t i = lo; i < hi; ++i) {
                    if (mii_typeid[i] != it2_id) continue;
                    std::string_view s(mii_info_data + mii_info_off[i],
                                       mii_info_off[i+1] - mii_info_off[i]);
                    if (!mii_local_set || s < mii_local) { mii_local = s; mii_local_set = true; }
                }
                if (!mii_local_set) continue;
            }

            // ci ⨝ name: project MIN(n.name) over writers with gender == 'm'
            std::string_view name_local;
            bool name_local_set = false;
            {
                uint32_t lo = off_ci[vv], hi = off_ci[vv+1];
                for (uint32_t i = lo; i < hi; ++i) {
                    const char* nd = ci_note_data + ci_note_off[i];
                    size_t nl = ci_note_off[i+1] - ci_note_off[i];
                    if (!ci_note_set.count(std::string_view(nd, nl))) continue;
                    int32_t pid = ci_person[i];
                    if (pid <= 0) continue;
                    size_t nrow = (size_t)pid - 1; // name.id is sequential 1..N
                    if (nrow >= N_name) continue;
                    if (name_gender[nrow] != gender_byte) continue;
                    std::string_view ns(name_data + name_off[nrow],
                                        name_off[nrow+1] - name_off[nrow]);
                    if (!name_local_set || ns < name_local) {
                        name_local = ns;
                        name_local_set = true;
                    }
                }
                if (!name_local_set) continue;
            }

            LocalMin& L = locals[tid];
            update_min(L.mi_min,    L.has_mi,    mi_local);
            update_min(L.mii_min,   L.has_mii,   mii_local);
            update_min(L.name_min,  L.has_name,  name_local);
            update_min(L.title_min, L.has_title, std::string_view(td, tlen));
        }
    }

    // ---------- Reduce ----------
    std::string mi_min, mii_min, name_min, title_min;
    bool has_mi=false, has_mii=false, has_name=false, has_title=false;
    for (auto& L : locals) {
        if (L.has_mi)    update_min(mi_min, has_mi, L.mi_min);
        if (L.has_mii)   update_min(mii_min, has_mii, L.mii_min);
        if (L.has_name)  update_min(name_min, has_name, L.name_min);
        if (L.has_title) update_min(title_min, has_title, L.title_min);
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q31b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open output %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "movie_budget,movie_votes,writer,violent_liongate_movie\n");
        auto put = [&](const std::string& s, bool has, bool last) {
            if (has) {
                bool need_quote = false;
                for (char c : s) {
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
                }
                if (need_quote) {
                    std::fputc('"', f);
                    for (char c : s) {
                        if (c == '"') std::fputc('"', f);
                        std::fputc(c, f);
                    }
                    std::fputc('"', f);
                } else {
                    std::fwrite(s.data(), 1, s.size(), f);
                }
            }
            std::fputc(last ? '\n' : ',', f);
        };
        put(mi_min, has_mi, false);
        put(mii_min, has_mii, false);
        put(name_min, has_name, false);
        put(title_min, has_title, true);
        std::fclose(f);
    }

    return 0;
}
