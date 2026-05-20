// Q30b — IMDB-JOB
// SELECT MIN(mi.info), MIN(mi_idx.info), MIN(n.name), MIN(t.title)
// Drive from title (LIKE filter), probe fact tables via offsets indexes.
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---- Helpers ----
static std::pair<const uint8_t*, size_t> map_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("open failed: " + path);
    struct stat st;
    if (fstat(fd, &st) < 0) { ::close(fd); throw std::runtime_error("fstat failed: " + path); }
    size_t sz = st.st_size;
    void* p = (sz == 0) ? nullptr : mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (sz != 0 && p == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
    return { static_cast<const uint8_t*>(p), sz };
}

static inline std::string_view sv_of(const uint8_t* dat, const int64_t* off, int64_t i) {
    int64_t lo = off[i], hi = off[i+1];
    return std::string_view(reinterpret_cast<const char*>(dat) + lo, hi - lo);
}

// memmem search (POSIX/glibc); fallback if not available
static inline const void* my_memmem(const void* hay, size_t hlen, const void* nee, size_t nlen) {
    return memmem(hay, hlen, nee, nlen);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    // ---------- Data loading ----------
    // dim columns
    MmapColumn<uint8_t> cct_kind_dat;
    MmapColumn<int64_t> cct_kind_off;
    MmapColumn<uint8_t> it_info_dat;
    MmapColumn<int64_t> it_info_off;
    MmapColumn<uint8_t> kw_dat;
    MmapColumn<int64_t> kw_off;
    MmapColumn<uint8_t> name_gender_dict_dat;
    MmapColumn<int64_t> name_gender_dict_off;
    MmapColumn<int8_t>  name_gender;
    MmapColumn<uint8_t> name_name_dat;
    MmapColumn<int64_t> name_name_off;

    // title
    MmapColumn<uint8_t> t_title_dat;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<int32_t> t_prod_year;

    // complete_cast
    MmapColumn<int32_t> cc_subject_id, cc_status_id;

    // movie_keyword
    MmapColumn<int32_t> mk_keyword_id;

    // movie_info
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<uint8_t> mi_info_dat;
    MmapColumn<int64_t> mi_info_off;

    // movie_info_idx
    MmapColumn<int32_t> mii_info_type_id;
    MmapColumn<uint8_t> mii_info_dat;
    MmapColumn<int64_t> mii_info_off;

    // cast_info
    MmapColumn<int32_t> ci_person_id;
    MmapColumn<uint8_t> ci_note_dat;
    MmapColumn<int64_t> ci_note_off;

    // indexes (offsets_only int32 arrays)
    MmapColumn<int32_t> idx_cc, idx_mk, idx_mi, idx_mii, idx_ci;

    {
        GENDB_PHASE("data_loading");
        cct_kind_dat.open(gendb_dir + "/comp_cast_type/kind.dat");
        cct_kind_off.open(gendb_dir + "/comp_cast_type/kind.off");
        it_info_dat.open(gendb_dir + "/info_type/info.dat");
        it_info_off.open(gendb_dir + "/info_type/info.off");
        kw_dat.open(gendb_dir + "/keyword/keyword.dat");
        kw_off.open(gendb_dir + "/keyword/keyword.off");

        name_gender_dict_dat.open(gendb_dir + "/name/gender.dict.dat");
        name_gender_dict_off.open(gendb_dir + "/name/gender.dict.off");
        name_gender.open(gendb_dir + "/name/gender.bin");
        name_name_dat.open(gendb_dir + "/name/name.dat");
        name_name_off.open(gendb_dir + "/name/name.off");

        t_title_dat.open(gendb_dir + "/title/title.dat");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_prod_year.open(gendb_dir + "/title/production_year.bin");

        cc_subject_id.open(gendb_dir + "/complete_cast/subject_id.bin");
        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");

        mk_keyword_id.open(gendb_dir + "/movie_keyword/keyword_id.bin");

        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");

        mii_info_type_id.open(gendb_dir + "/movie_info_idx/info_type_id.bin");
        mii_info_dat.open(gendb_dir + "/movie_info_idx/info.dat");
        mii_info_off.open(gendb_dir + "/movie_info_idx/info.off");

        ci_person_id.open(gendb_dir + "/cast_info/person_id.bin");
        ci_note_dat.open(gendb_dir + "/cast_info/note.dat");
        ci_note_off.open(gendb_dir + "/cast_info/note.off");

        idx_cc.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        idx_mk.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        idx_mi.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");
        idx_mii.open(gendb_dir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        idx_ci.open(gendb_dir + "/_idx/cast_info__movie_id__offsets.bin");
    }

    // ---------- Resolve dims ----------
    int32_t cct1_ids[4]; int n_cct1 = 0;
    int32_t cct2_id = -1;
    {
        size_t n = cct_kind_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            auto s = sv_of(cct_kind_dat.data, cct_kind_off.data, i);
            if (s == "cast" || s == "crew") {
                cct1_ids[n_cct1++] = (int32_t)(i + 1);
            } else if (s == "complete+verified") {
                cct2_id = (int32_t)(i + 1);
            }
        }
    }

    int32_t it1_id = -1, it2_id = -1;
    {
        size_t n = it_info_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            auto s = sv_of(it_info_dat.data, it_info_off.data, i);
            if (s == "genres") it1_id = (int32_t)(i + 1);
            else if (s == "votes") it2_id = (int32_t)(i + 1);
        }
    }

    int32_t k_ids[16]; int n_k = 0;
    {
        static const char* kws[] = {"murder","violence","blood","gore","death","female-nudity","hospital"};
        size_t n = kw_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            auto s = sv_of(kw_dat.data, kw_off.data, i);
            for (int j = 0; j < 7; ++j) {
                size_t ln = std::strlen(kws[j]);
                if (s.size() == ln && memcmp(s.data(), kws[j], ln) == 0) {
                    k_ids[n_k++] = (int32_t)(i + 1);
                    break;
                }
            }
        }
    }

    // Resolve gender code for 'm'
    int8_t m_code = 0;
    {
        size_t n = name_gender_dict_off.count - 1;
        for (size_t i = 0; i < n; ++i) {
            auto s = sv_of(name_gender_dict_dat.data, name_gender_dict_off.data, i);
            if (s.size() == 1 && s[0] == 'm') { m_code = (int8_t)(i + 1); break; }
        }
    }

    if (n_cct1 == 0 || cct2_id < 0 || it1_id < 0 || it2_id < 0 || n_k == 0 || m_code == 0) {
        std::fprintf(stderr, "Dimension resolution failed.\n");
        return 1;
    }

    // ---------- Title filter (parallel scan) ----------
    std::vector<int32_t> mv_set;
    {
        GENDB_PHASE("main_scan");
        size_t T = t_title_off.count - 1; // 2,528,312
        const int64_t* toff = t_title_off.data;
        const uint8_t* tdat = t_title_dat.data;
        const int32_t* tpy  = t_prod_year.data;

        int nthr = omp_get_max_threads();
        std::vector<std::vector<int32_t>> local_mvs(nthr);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& local = local_mvs[tid];
            local.reserve(64);
            #pragma omp for schedule(static)
            for (size_t i = 0; i < T; ++i) {
                int32_t py = tpy[i];
                if (py <= 2000) continue;  // also rejects INT32_MIN (null)
                int64_t lo = toff[i], hi = toff[i+1];
                size_t len = (size_t)(hi - lo);
                const uint8_t* s = tdat + lo;
                bool match = false;
                if (len >= 6) {
                    if (my_memmem(s, len, "Freddy", 6)) match = true;
                    else if (my_memmem(s, len, "Jason", 5)) match = true;
                }
                if (!match && len >= 3 && memcmp(s, "Saw", 3) == 0) match = true;
                if (match) {
                    local.push_back((int32_t)(i + 1));
                }
            }
        }
        // Merge
        size_t total = 0;
        for (auto& v : local_mvs) total += v.size();
        mv_set.reserve(total);
        for (auto& v : local_mvs) mv_set.insert(mv_set.end(), v.begin(), v.end());
        std::sort(mv_set.begin(), mv_set.end());
    }

    // ---------- Probe fact tables and aggregate ----------
    // Writer notes
    static const std::string_view writer_notes[5] = {
        std::string_view("(writer)", 8),
        std::string_view("(head writer)", 13),
        std::string_view("(written by)", 12),
        std::string_view("(story)", 7),
        std::string_view("(story editor)", 14),
    };
    auto is_writer_note = [&](std::string_view s) {
        for (int i = 0; i < 5; ++i) {
            if (s.size() == writer_notes[i].size() &&
                memcmp(s.data(), writer_notes[i].data(), s.size()) == 0) return true;
        }
        return false;
    };

    std::string min_mi_info;        bool has_mi_info = false;
    std::string min_mii_info;       bool has_mii_info = false;
    std::string min_name;           bool has_name = false;
    std::string min_title;          bool has_title = false;

    auto upd = [](bool& has, std::string& cur, std::string_view cand) {
        if (!has) { cur.assign(cand.data(), cand.size()); has = true; return; }
        if (cand < std::string_view(cur)) { cur.assign(cand.data(), cand.size()); }
    };

    {
        GENDB_PHASE("probes_aggregate");
        for (int32_t mv : mv_set) {
            // 1) complete_cast probe
            int32_t lo, hi;
            lo = idx_cc.data[mv]; hi = idx_cc.data[mv+1];
            bool cc_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                int32_t st = cc_status_id.data[r];
                if (st != cct2_id) continue;
                int32_t sub = cc_subject_id.data[r];
                bool in_set = false;
                for (int j = 0; j < n_cct1; ++j) {
                    if (sub == cct1_ids[j]) { in_set = true; break; }
                }
                if (in_set) { cc_ok = true; break; }
            }
            if (!cc_ok) continue;

            // 2) movie_keyword probe
            lo = idx_mk.data[mv]; hi = idx_mk.data[mv+1];
            bool mk_ok = false;
            for (int32_t r = lo; r < hi; ++r) {
                int32_t kid = mk_keyword_id.data[r];
                for (int j = 0; j < n_k; ++j) {
                    if (kid == k_ids[j]) { mk_ok = true; break; }
                }
                if (mk_ok) break;
            }
            if (!mk_ok) continue;

            // 3) movie_info probe — find any row with it1_id and info in {Horror,Thriller}
            lo = idx_mi.data[mv]; hi = idx_mi.data[mv+1];
            // Collect candidate info string_views; only update MIN once we know all probes pass.
            // Defer the update: capture local min for this mv first.
            std::string_view local_mi_min;
            bool local_mi_has = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (mi_info_type_id.data[r] != it1_id) continue;
                auto s = sv_of(mi_info_dat.data, mi_info_off.data, r);
                bool ok = false;
                if (s.size() == 6 && memcmp(s.data(), "Horror", 6) == 0) ok = true;
                else if (s.size() == 8 && memcmp(s.data(), "Thriller", 8) == 0) ok = true;
                if (!ok) continue;
                if (!local_mi_has) { local_mi_min = s; local_mi_has = true; }
                else if (s < local_mi_min) local_mi_min = s;
            }
            if (!local_mi_has) continue;

            // 4) movie_info_idx probe
            lo = idx_mii.data[mv]; hi = idx_mii.data[mv+1];
            std::string_view local_mii_min;
            bool local_mii_has = false;
            for (int32_t r = lo; r < hi; ++r) {
                if (mii_info_type_id.data[r] != it2_id) continue;
                auto s = sv_of(mii_info_dat.data, mii_info_off.data, r);
                if (!local_mii_has) { local_mii_min = s; local_mii_has = true; }
                else if (s < local_mii_min) local_mii_min = s;
            }
            if (!local_mii_has) continue;

            // 5) cast_info probe — for each writer-note row, gender == m
            lo = idx_ci.data[mv]; hi = idx_ci.data[mv+1];
            std::string_view local_name_min;
            bool local_name_has = false;
            for (int32_t r = lo; r < hi; ++r) {
                auto note = sv_of(ci_note_dat.data, ci_note_off.data, r);
                if (!is_writer_note(note)) continue;
                int32_t pid = ci_person_id.data[r];
                if (pid <= 0) continue;
                int8_t g = name_gender.data[pid - 1];
                if (g != m_code) continue;
                auto nm = sv_of(name_name_dat.data, name_name_off.data, pid - 1);
                if (!local_name_has) { local_name_min = nm; local_name_has = true; }
                else if (nm < local_name_min) local_name_min = nm;
            }
            if (!local_name_has) continue;

            // All checks passed for this mv — update global MINs
            upd(has_mi_info, min_mi_info, local_mi_min);
            upd(has_mii_info, min_mii_info, local_mii_min);
            upd(has_name, min_name, local_name_min);

            // Title for this mv
            auto t = sv_of(t_title_dat.data, t_title_off.data, mv - 1);
            upd(has_title, min_title, t);
        }
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q30b.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Cannot open %s for write\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_budget,movie_votes,writer,complete_gore_movie\n");

        auto write_field = [&](const std::string& s, bool has) {
            if (!has) return;
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
        };

        write_field(min_mi_info, has_mi_info);  std::fputc(',', f);
        write_field(min_mii_info, has_mii_info); std::fputc(',', f);
        write_field(min_name, has_name);        std::fputc(',', f);
        write_field(min_title, has_title);      std::fputc('\n', f);
        std::fclose(f);
    }

    return 0;
}
