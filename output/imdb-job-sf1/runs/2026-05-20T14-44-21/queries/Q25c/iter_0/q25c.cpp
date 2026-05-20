// Q25c — implementation per plan
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <string_view>
#include <algorithm>
#include <omp.h>
#include "timing_utils.h"

struct Mapped {
    const void* ptr = nullptr;
    size_t size = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "Cannot open %s\n", path.c_str()); std::exit(1); }
    struct stat st;
    if (fstat(fd, &st) < 0) { std::fprintf(stderr, "fstat %s\n", path.c_str()); std::exit(1); }
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { std::fprintf(stderr, "mmap %s\n", path.c_str()); std::exit(1); }
    // Hint kernel: sequential / will_need where helpful
    madvise(p, st.st_size, MADV_RANDOM);
    return {p, (size_t)st.st_size};
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s gendb_dir results_dir\n", argv[0]);
        return 1;
    }
    const std::string g = argv[1];
    const std::string r = argv[2];

    {
        std::string cmd = "mkdir -p \"" + r + "\"";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    // ===== mmap =====
    Mapped m_title_off, m_title_dat;
    Mapped m_mk_off, m_mk_kid;
    Mapped m_miidx_off, m_miidx_itid, m_miidx_info_off, m_miidx_info_dat;
    Mapped m_mi_off, m_mi_itid, m_mi_info_off, m_mi_info_dat;
    Mapped m_ci_off, m_ci_pid, m_ci_note_off, m_ci_note_dat;
    Mapped m_name_off, m_name_dat, m_gender;
    Mapped m_it_id, m_it_info_off, m_it_info_dat;
    Mapped m_kw_id, m_kw_off, m_kw_dat;

    {
        GENDB_PHASE("data_loading");
        m_title_off       = map_file(g + "/title/title.off");
        m_title_dat       = map_file(g + "/title/title.dat");
        m_mk_off          = map_file(g + "/_idx/movie_keyword__movie_id__offsets.bin");
        m_mk_kid          = map_file(g + "/movie_keyword/keyword_id.bin");
        m_miidx_off       = map_file(g + "/_idx/movie_info_idx__movie_id__offsets.bin");
        m_miidx_itid      = map_file(g + "/movie_info_idx/info_type_id.bin");
        m_miidx_info_off  = map_file(g + "/movie_info_idx/info.off");
        m_miidx_info_dat  = map_file(g + "/movie_info_idx/info.dat");
        m_mi_off          = map_file(g + "/_idx/movie_info__movie_id__offsets.bin");
        m_mi_itid         = map_file(g + "/movie_info/info_type_id.bin");
        m_mi_info_off     = map_file(g + "/movie_info/info.off");
        m_mi_info_dat     = map_file(g + "/movie_info/info.dat");
        m_ci_off          = map_file(g + "/_idx/cast_info__movie_id__offsets.bin");
        m_ci_pid          = map_file(g + "/cast_info/person_id.bin");
        m_ci_note_off     = map_file(g + "/cast_info/note.off");
        m_ci_note_dat     = map_file(g + "/cast_info/note.dat");
        m_name_off        = map_file(g + "/name/name.off");
        m_name_dat        = map_file(g + "/name/name.dat");
        m_gender          = map_file(g + "/name/gender.bin");
        m_it_id           = map_file(g + "/info_type/id.bin");
        m_it_info_off     = map_file(g + "/info_type/info.off");
        m_it_info_dat     = map_file(g + "/info_type/info.dat");
        m_kw_id           = map_file(g + "/keyword/id.bin");
        m_kw_off          = map_file(g + "/keyword/keyword.off");
        m_kw_dat          = map_file(g + "/keyword/keyword.dat");
    }

    const int64_t* title_off       = (const int64_t*)m_title_off.ptr;
    const char*    title_dat       = (const char*)m_title_dat.ptr;
    const int32_t* mk_off          = (const int32_t*)m_mk_off.ptr;
    const int32_t* mk_kid          = (const int32_t*)m_mk_kid.ptr;
    const int32_t* miidx_off       = (const int32_t*)m_miidx_off.ptr;
    const int32_t* miidx_itid      = (const int32_t*)m_miidx_itid.ptr;
    const int64_t* miidx_info_off  = (const int64_t*)m_miidx_info_off.ptr;
    const char*    miidx_info_dat  = (const char*)m_miidx_info_dat.ptr;
    const int32_t* mi_off          = (const int32_t*)m_mi_off.ptr;
    const int32_t* mi_itid         = (const int32_t*)m_mi_itid.ptr;
    const int64_t* mi_info_off     = (const int64_t*)m_mi_info_off.ptr;
    const char*    mi_info_dat     = (const char*)m_mi_info_dat.ptr;
    const int32_t* ci_off          = (const int32_t*)m_ci_off.ptr;
    const int32_t* ci_pid          = (const int32_t*)m_ci_pid.ptr;
    const int64_t* ci_note_off     = (const int64_t*)m_ci_note_off.ptr;
    const char*    ci_note_dat     = (const char*)m_ci_note_dat.ptr;
    const int64_t* name_off        = (const int64_t*)m_name_off.ptr;
    const char*    name_dat        = (const char*)m_name_dat.ptr;
    const int8_t*  gender          = (const int8_t*)m_gender.ptr;
    const int32_t* it_id           = (const int32_t*)m_it_id.ptr;
    const int64_t* it_info_off     = (const int64_t*)m_it_info_off.ptr;
    const char*    it_info_dat     = (const char*)m_it_info_dat.ptr;
    const int32_t* kw_id           = (const int32_t*)m_kw_id.ptr;
    const int64_t* kw_off          = (const int64_t*)m_kw_off.ptr;
    const char*    kw_dat          = (const char*)m_kw_dat.ptr;

    const int32_t n_title = 2528312;
    const int32_t n_name  = 4167491;
    const int32_t n_it    = 113;
    const int32_t n_kw    = 134170;

    // ===== Pre-pass: info_type literals and keyword ids =====
    int32_t it1_id = -1, it2_id = -1;
    int32_t kw_set[16];
    int     num_kw = 0;

    {
        GENDB_PHASE("prepass_dims");
        for (int32_t i = 0; i < n_it; ++i) {
            int64_t lo = it_info_off[i], hi = it_info_off[i+1];
            int len = (int)(hi - lo);
            const char* s = it_info_dat + lo;
            if (len == 6 && std::memcmp(s, "genres", 6) == 0) it1_id = it_id[i];
            else if (len == 5 && std::memcmp(s, "votes", 5) == 0) it2_id = it_id[i];
        }
        if (it1_id < 0 || it2_id < 0) {
            std::fprintf(stderr, "info_type lookup failed: it1=%d it2=%d\n", it1_id, it2_id);
            return 1;
        }

        static const char* kws[] = {
            "murder","violence","blood","gore","death","female-nudity","hospital"
        };
        static const int kw_lens[] = {6,8,5,4,5,13,8};
        for (int32_t i = 0; i < n_kw; ++i) {
            int64_t lo = kw_off[i], hi = kw_off[i+1];
            int len = (int)(hi - lo);
            const char* s = kw_dat + lo;
            for (int j = 0; j < 7; ++j) {
                if (len == kw_lens[j] && std::memcmp(s, kws[j], len) == 0) {
                    kw_set[num_kw++] = kw_id[i];
                    break;
                }
            }
        }
    }

    // ===== gender code resolution =====
    // Dict encoding uses 1-based codes (code 0 = NULL, code k = dict entry k-1).
    // dict.dat = "mf"; so 'm' is dict entry 0 → code 1.
    int8_t m_code = -1;
    {
        Mapped d_dat = map_file(g + "/name/gender.dict.dat");
        Mapped d_off = map_file(g + "/name/gender.dict.off");
        const char* dict_dat = (const char*)d_dat.ptr;
        const int64_t* dict_off = (const int64_t*)d_off.ptr;
        int n_entries = (int)(d_off.size / 8) - 1;
        for (int i = 0; i < n_entries; ++i) {
            int64_t lo = dict_off[i], hi = dict_off[i+1];
            int len = (int)(hi - lo);
            if (len == 1 && dict_dat[lo] == 'm') { m_code = (int8_t)(i + 1); break; }
        }
        if (m_code < 0) {
            std::fprintf(stderr, "m_code lookup failed\n");
            return 1;
        }
    }

    // ===== Pre-pass: valid_persons bitset =====
    const int32_t n_words = (n_name + 63) / 64;
    std::vector<uint64_t> valid_persons(n_words, 0);
    {
        GENDB_PHASE("prepass_valid_persons");
        for (int32_t i = 0; i < n_name; ++i) {
            if (gender[i] == m_code) {
                valid_persons[i >> 6] |= (1ULL << (i & 63));
            }
        }
    }

    // ===== Filter literals =====
    static const char* notes[] = {
        "(writer)","(head writer)","(written by)","(story)","(story editor)"
    };
    static const int note_lens[] = {8,13,12,7,14};

    static const char* mi_infos[] = {"Horror","Action","Sci-Fi","Thriller","Crime","War"};
    static const int mi_info_lens[] = {6,6,6,8,5,3};

    // ===== Per-thread mins =====
    struct Mins {
        std::string mi_info;
        std::string mi_idx_info;
        std::string n_name;
        std::string t_title;
        bool has_mi = false, has_miidx = false, has_n = false, has_t = false;
    };

    int nthreads = omp_get_max_threads();
    std::vector<Mins> tmins(nthreads);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel for schedule(dynamic, 4096)
        for (int32_t v = 1; v <= n_title; ++v) {
            // --- mk gate ---
            int32_t mk_lo = mk_off[v], mk_hi = mk_off[v+1];
            bool mk_match = false;
            for (int32_t rr = mk_lo; rr < mk_hi; ++rr) {
                int32_t kid = mk_kid[rr];
                for (int j = 0; j < num_kw; ++j) {
                    if (kid == kw_set[j]) { mk_match = true; break; }
                }
                if (mk_match) break;
            }
            if (!mk_match) continue;

            // --- mi_idx gate ---
            int32_t miidx_lo = miidx_off[v], miidx_hi = miidx_off[v+1];
            bool miidx_match = false;
            for (int32_t rr = miidx_lo; rr < miidx_hi; ++rr) {
                if (miidx_itid[rr] == it2_id) { miidx_match = true; break; }
            }
            if (!miidx_match) continue;

            // --- mi gate (info_type_id == it1_id AND info ∈ set) ---
            int32_t mi_lo = mi_off[v], mi_hi = mi_off[v+1];
            bool mi_match = false;
            for (int32_t rr = mi_lo; rr < mi_hi; ++rr) {
                if (mi_itid[rr] != it1_id) continue;
                int64_t io = mi_info_off[rr], ie = mi_info_off[rr+1];
                int ilen = (int)(ie - io);
                const char* is = mi_info_dat + io;
                for (int j = 0; j < 6; ++j) {
                    if (ilen == mi_info_lens[j] && std::memcmp(is, mi_infos[j], ilen) == 0) {
                        mi_match = true; break;
                    }
                }
                if (mi_match) break;
            }
            if (!mi_match) continue;

            // --- ci gate (note in writer-set AND valid_persons[pid-1]) ---
            int32_t ci_lo = ci_off[v], ci_hi = ci_off[v+1];
            bool ci_match = false;
            for (int32_t rr = ci_lo; rr < ci_hi; ++rr) {
                int64_t no = ci_note_off[rr], ne = ci_note_off[rr+1];
                int nlen = (int)(ne - no);
                const char* ns = ci_note_dat + no;
                bool note_ok = false;
                for (int j = 0; j < 5; ++j) {
                    if (nlen == note_lens[j] && std::memcmp(ns, notes[j], nlen) == 0) {
                        note_ok = true; break;
                    }
                }
                if (!note_ok) continue;
                int32_t pid = ci_pid[rr];
                if (pid < 1 || pid > n_name) continue;
                int32_t idx = pid - 1;
                if (valid_persons[idx >> 6] & (1ULL << (idx & 63))) {
                    ci_match = true; break;
                }
            }
            if (!ci_match) continue;

            // ===== All 4 gates pass — update MINs =====
            int tid = omp_get_thread_num();
            Mins& M = tmins[tid];

            // t.title (v is 1-based; title_off[0..n_title])
            {
                int64_t to = title_off[v-1], te = title_off[v];
                std::string_view sv(title_dat + to, te - to);
                if (!M.has_t || sv < M.t_title) { M.t_title.assign(sv); M.has_t = true; }
            }

            // MIN(mi.info) across all matching mi rows
            for (int32_t rr = mi_lo; rr < mi_hi; ++rr) {
                if (mi_itid[rr] != it1_id) continue;
                int64_t io = mi_info_off[rr], ie = mi_info_off[rr+1];
                int ilen = (int)(ie - io);
                const char* is = mi_info_dat + io;
                bool ok = false;
                for (int j = 0; j < 6; ++j) {
                    if (ilen == mi_info_lens[j] && std::memcmp(is, mi_infos[j], ilen) == 0) {
                        ok = true; break;
                    }
                }
                if (!ok) continue;
                std::string_view sv(is, ilen);
                if (!M.has_mi || sv < M.mi_info) { M.mi_info.assign(sv); M.has_mi = true; }
            }

            // MIN(mi_idx.info) across all matching mi_idx rows
            for (int32_t rr = miidx_lo; rr < miidx_hi; ++rr) {
                if (miidx_itid[rr] != it2_id) continue;
                int64_t io = miidx_info_off[rr], ie = miidx_info_off[rr+1];
                std::string_view sv(miidx_info_dat + io, ie - io);
                if (!M.has_miidx || sv < M.mi_idx_info) { M.mi_idx_info.assign(sv); M.has_miidx = true; }
            }

            // MIN(n.name) across all matching ci rows
            for (int32_t rr = ci_lo; rr < ci_hi; ++rr) {
                int64_t no = ci_note_off[rr], ne = ci_note_off[rr+1];
                int nlen = (int)(ne - no);
                const char* ns = ci_note_dat + no;
                bool note_ok = false;
                for (int j = 0; j < 5; ++j) {
                    if (nlen == note_lens[j] && std::memcmp(ns, notes[j], nlen) == 0) {
                        note_ok = true; break;
                    }
                }
                if (!note_ok) continue;
                int32_t pid = ci_pid[rr];
                if (pid < 1 || pid > n_name) continue;
                int32_t idx = pid - 1;
                if (!(valid_persons[idx >> 6] & (1ULL << (idx & 63)))) continue;
                int64_t no2 = name_off[idx], ne2 = name_off[idx+1];
                std::string_view nm(name_dat + no2, ne2 - no2);
                if (!M.has_n || nm < M.n_name) { M.n_name.assign(nm); M.has_n = true; }
            }
        }
    }

    // ===== Merge per-thread results =====
    Mins F;
    for (auto& M : tmins) {
        if (M.has_mi    && (!F.has_mi    || M.mi_info     < F.mi_info))     { F.mi_info     = M.mi_info;     F.has_mi    = true; }
        if (M.has_miidx && (!F.has_miidx || M.mi_idx_info < F.mi_idx_info)) { F.mi_idx_info = M.mi_idx_info; F.has_miidx = true; }
        if (M.has_n     && (!F.has_n     || M.n_name      < F.n_name))      { F.n_name      = M.n_name;      F.has_n     = true; }
        if (M.has_t     && (!F.has_t     || M.t_title     < F.t_title))     { F.t_title     = M.t_title;     F.has_t     = true; }
    }

    // ===== Output CSV =====
    {
        GENDB_PHASE("output");
        std::string out_path = r + "/Q25c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "Cannot open %s\n", out_path.c_str()); return 1; }
        std::fprintf(f, "movie_budget,movie_votes,male_writer,violent_movie_title\n");

        auto write_field = [&](const std::string& s, bool empty_ok, bool trailing_comma) {
            bool need_quote = s.find(',') != std::string::npos ||
                              s.find('"') != std::string::npos ||
                              s.find('\n') != std::string::npos;
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
            if (trailing_comma) std::fputc(',', f);
            (void)empty_ok;
        };

        if (F.has_mi || F.has_miidx || F.has_n || F.has_t) {
            write_field(F.mi_info,     true, true);
            write_field(F.mi_idx_info, true, true);
            write_field(F.n_name,      true, true);
            write_field(F.t_title,     true, false);
            std::fputc('\n', f);
        }
        std::fclose(f);
    }

    return 0;
}
