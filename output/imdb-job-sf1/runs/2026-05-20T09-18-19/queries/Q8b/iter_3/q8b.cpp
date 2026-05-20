// Q8b — Japanese anime, voice actresses, MIN(aka.name), MIN(title)
#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

struct Mapped {
    void* ptr = nullptr;
    size_t size = 0;
    int fd = -1;
    ~Mapped() {
        if (ptr && size) munmap(ptr, size);
        if (fd >= 0) ::close(fd);
    }
};

static bool mmap_file(const std::string& path, Mapped& m) {
    m.fd = ::open(path.c_str(), O_RDONLY);
    if (m.fd < 0) return false;
    struct stat st;
    if (fstat(m.fd, &st) < 0) return false;
    m.size = st.st_size;
    if (m.size == 0) { m.ptr = nullptr; return true; }
    m.ptr = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.fd, 0);
    if (m.ptr == MAP_FAILED) { m.ptr = nullptr; return false; }
    madvise(m.ptr, m.size, MADV_SEQUENTIAL);
    return true;
}

// fast substring scan
static inline bool contains(const char* s, size_t n, const char* pat, size_t plen) {
    if (plen == 0) return true;
    if (n < plen) return false;
    return memmem(s, n, pat, plen) != nullptr;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    std::filesystem::create_directories(results_dir);

    GENDB_PHASE("total");

    std::string min_aka, min_title;
    bool have_aka = false, have_title = false;

    // ----------------------------------------------------------------------
    // Data loading: mmap all needed files
    // ----------------------------------------------------------------------
    Mapped rt_off_m, rt_dat_m, cc_off_m, cc_dat_m, cc_bin_m;
    Mapped t_off_m, t_dat_m, t_year_m;
    Mapped mc_mid_off_m, ci_mid_off_m, ak_pid_off_m;
    Mapped mc_note_off_m, mc_note_dat_m, mc_cid_m;
    Mapped ci_note_off_m, ci_note_dat_m, ci_role_m, ci_pid_m;
    Mapped n_off_m, n_dat_m;
    Mapped ak_name_off_m, ak_name_dat_m;

    int32_t rt_id_actress = 0;
    int16_t jp_code = 0;
    size_t N_title = 0, N_company = 0, N_name = 0;

    {
        GENDB_PHASE("data_loading");
        mmap_file(store + "/role_type/role.off", rt_off_m);
        mmap_file(store + "/role_type/role.dat", rt_dat_m);
        mmap_file(store + "/company_name/country_code.dict.off", cc_off_m);
        mmap_file(store + "/company_name/country_code.dict.dat", cc_dat_m);
        mmap_file(store + "/company_name/country_code.bin", cc_bin_m);

        mmap_file(store + "/title/title.off", t_off_m);
        mmap_file(store + "/title/title.dat", t_dat_m);
        mmap_file(store + "/title/production_year.bin", t_year_m);

        mmap_file(store + "/_idx/movie_companies__movie_id__offsets.bin", mc_mid_off_m);
        mmap_file(store + "/_idx/cast_info__movie_id__offsets.bin", ci_mid_off_m);
        mmap_file(store + "/_idx/aka_name__person_id__offsets.bin", ak_pid_off_m);

        mmap_file(store + "/movie_companies/note.off", mc_note_off_m);
        mmap_file(store + "/movie_companies/note.dat", mc_note_dat_m);
        mmap_file(store + "/movie_companies/company_id.bin", mc_cid_m);

        mmap_file(store + "/cast_info/note.off", ci_note_off_m);
        mmap_file(store + "/cast_info/note.dat", ci_note_dat_m);
        mmap_file(store + "/cast_info/role_id.bin", ci_role_m);
        mmap_file(store + "/cast_info/person_id.bin", ci_pid_m);

        mmap_file(store + "/name/name.off", n_off_m);
        mmap_file(store + "/name/name.dat", n_dat_m);

        mmap_file(store + "/aka_name/name.off", ak_name_off_m);
        mmap_file(store + "/aka_name/name.dat", ak_name_dat_m);

        N_title = t_year_m.size / sizeof(int32_t);
        N_company = cc_bin_m.size / sizeof(int16_t);
        N_name = (n_off_m.size / sizeof(int64_t)) - 1;
    }

    // ----------------------------------------------------------------------
    // Step 1: resolve rt_id_actress, jp_code
    // ----------------------------------------------------------------------
    {
        GENDB_PHASE("resolve_codes");
        const int64_t* rt_off = (const int64_t*)rt_off_m.ptr;
        const char* rt_dat = (const char*)rt_dat_m.ptr;
        size_t nrt = (rt_off_m.size / sizeof(int64_t)) - 1;
        for (size_t i = 0; i < nrt; ++i) {
            size_t len = rt_off[i+1] - rt_off[i];
            if (len == 7 && memcmp(rt_dat + rt_off[i], "actress", 7) == 0) {
                rt_id_actress = (int32_t)(i + 1);
                break;
            }
        }

        const int64_t* cc_off = (const int64_t*)cc_off_m.ptr;
        const char* cc_dat = (const char*)cc_dat_m.ptr;
        size_t ncc = (cc_off_m.size / sizeof(int64_t)) - 1;
        for (size_t i = 0; i < ncc; ++i) {
            size_t len = cc_off[i+1] - cc_off[i];
            if (len == 4 && memcmp(cc_dat + cc_off[i], "[jp]", 4) == 0) {
                jp_code = (int16_t)(i + 1);
                break;
            }
        }
    }

    if (rt_id_actress == 0 || jp_code == 0) {
        std::fprintf(stderr, "Failed to resolve rt_id_actress or jp_code\n");
        return 1;
    }

    // ----------------------------------------------------------------------
    // Step 2: build cn_is_jp[1..N_company]
    // ----------------------------------------------------------------------
    std::vector<uint8_t> cn_is_jp(N_company + 1, 0);
    {
        GENDB_PHASE("build_cn_is_jp");
        const int16_t* cc = (const int16_t*)cc_bin_m.ptr;
        for (size_t i = 0; i < N_company; ++i) {
            cn_is_jp[i+1] = (cc[i] == jp_code) ? 1u : 0u;
        }
    }

    // ----------------------------------------------------------------------
    // Step 3: scan title — collect t_ids matching production_year + prefix LIKE
    // ----------------------------------------------------------------------
    struct TDrv { int32_t t_id; const char* title_ptr; size_t title_len; };
    std::vector<TDrv> drivers;
    {
        GENDB_PHASE("title_driver");
        const int32_t* year = (const int32_t*)t_year_m.ptr;
        const int64_t* toff = (const int64_t*)t_off_m.ptr;
        const char* tdat = (const char*)t_dat_m.ptr;
        const char op[] = "One Piece";
        const char dbz[] = "Dragon Ball Z";
        constexpr size_t op_len = sizeof(op) - 1;
        constexpr size_t dbz_len = sizeof(dbz) - 1;
        for (size_t i = 0; i < N_title; ++i) {
            int32_t y = year[i];
            if (y != 2006 && y != 2007) continue;
            size_t len = toff[i+1] - toff[i];
            const char* p = tdat + toff[i];
            bool ok = (len >= op_len && memcmp(p, op, op_len) == 0)
                   || (len >= dbz_len && memcmp(p, dbz, dbz_len) == 0);
            if (!ok) continue;
            drivers.push_back({(int32_t)(i+1), p, len});
        }
    }

    // ----------------------------------------------------------------------
    // Step 4: build name_ok bitset (scan all 4.17M names once)
    // ----------------------------------------------------------------------
    std::vector<uint8_t> name_ok(N_name + 1, 0);
    {
        GENDB_PHASE("build_name_ok");
        const int64_t* noff = (const int64_t*)n_off_m.ptr;
        const char* ndat = (const char*)n_dat_m.ptr;
        for (size_t i = 0; i < N_name; ++i) {
            size_t off0 = noff[i];
            size_t len = noff[i+1] - off0;
            const char* p = ndat + off0;
            if (contains(p, len, "Yo", 2) && !contains(p, len, "Yu", 2)) {
                name_ok[i+1] = 1;
            }
        }
    }

    // ----------------------------------------------------------------------
    // Step 5: per-title — probe mc, then ci, then aka_name
    // ----------------------------------------------------------------------
    {
        GENDB_PHASE("main_scan");
        const int32_t* mc_off = (const int32_t*)mc_mid_off_m.ptr;
        const int64_t* mc_note_off = (const int64_t*)mc_note_off_m.ptr;
        const char* mc_note_dat = (const char*)mc_note_dat_m.ptr;
        const int32_t* mc_cid = (const int32_t*)mc_cid_m.ptr;

        const int32_t* ci_off = (const int32_t*)ci_mid_off_m.ptr;
        const int64_t* ci_note_off = (const int64_t*)ci_note_off_m.ptr;
        const char* ci_note_dat = (const char*)ci_note_dat_m.ptr;
        const int32_t* ci_role = (const int32_t*)ci_role_m.ptr;
        const int32_t* ci_pid = (const int32_t*)ci_pid_m.ptr;

        const int32_t* ak_off = (const int32_t*)ak_pid_off_m.ptr;
        const int64_t* ak_name_off = (const int64_t*)ak_name_off_m.ptr;
        const char* ak_name_dat = (const char*)ak_name_dat_m.ptr;

        static const char VOICE_NOTE[] = "(voice: English version)";
        constexpr size_t VOICE_LEN = sizeof(VOICE_NOTE) - 1;

        for (const auto& td : drivers) {
            int32_t t_id = td.t_id;

            // (a) mc range: at least one mc row must satisfy LIKE filters + jp
            int32_t mlo = mc_off[t_id], mhi = mc_off[t_id+1];
            bool mc_ok = false;
            for (int32_t r = mlo; r < mhi; ++r) {
                // cn_is_jp[mc.company_id]
                int32_t cid = mc_cid[r];
                if (cid <= 0 || (size_t)cid > N_company || !cn_is_jp[cid]) continue;
                size_t off0 = mc_note_off[r];
                size_t nl = mc_note_off[r+1] - off0;
                const char* np = mc_note_dat + off0;
                if (!contains(np, nl, "(Japan)", 7)) continue;
                if (contains(np, nl, "(USA)", 5)) continue;
                if (!(contains(np, nl, "(2006)", 6) || contains(np, nl, "(2007)", 6))) continue;
                mc_ok = true;
                break;
            }
            if (!mc_ok) continue;

            // (b) ci range
            int32_t clo = ci_off[t_id], chi = ci_off[t_id+1];
            for (int32_t r = clo; r < chi; ++r) {
                if (ci_role[r] != rt_id_actress) continue;
                size_t off0 = ci_note_off[r];
                size_t nl = ci_note_off[r+1] - off0;
                if (nl != VOICE_LEN) continue;
                if (memcmp(ci_note_dat + off0, VOICE_NOTE, VOICE_LEN) != 0) continue;

                int32_t pid = ci_pid[r];
                if (pid <= 0 || (size_t)pid > N_name) continue;
                if (!name_ok[pid]) continue;

                // (c) aka_name range
                int32_t alo = ak_off[pid], ahi = ak_off[pid+1];
                for (int32_t a = alo; a < ahi; ++a) {
                    size_t aoff = ak_name_off[a];
                    size_t alen = ak_name_off[a+1] - aoff;
                    std::string_view aname(ak_name_dat + aoff, alen);
                    if (!have_aka || aname < std::string_view(min_aka)) {
                        min_aka.assign(aname);
                        have_aka = true;
                    }
                    std::string_view ttl(td.title_ptr, td.title_len);
                    if (!have_title || ttl < std::string_view(min_title)) {
                        min_title.assign(ttl);
                        have_title = true;
                    }
                }
            }
        }
    }

    // ----------------------------------------------------------------------
    // Step 6: write CSV
    // ----------------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string outpath = results_dir + "/Q8b.csv";
        FILE* f = std::fopen(outpath.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", outpath.c_str());
            return 1;
        }
        std::fprintf(f, "acress_pseudonym,japanese_anime_movie\n");
        auto quote_if_needed = [](const std::string& s) -> std::string {
            bool need = s.find(',') != std::string::npos
                     || s.find('"') != std::string::npos
                     || s.find('\n') != std::string::npos;
            if (!need) return s;
            std::string out = "\"";
            for (char c : s) {
                if (c == '"') out += "\"\"";
                else out += c;
            }
            out += "\"";
            return out;
        };
        std::string a = have_aka ? quote_if_needed(min_aka) : std::string();
        std::string t = have_title ? quote_if_needed(min_title) : std::string();
        std::fprintf(f, "%s,%s\n", a.c_str(), t.c_str());
        std::fclose(f);
    }

    return 0;
}
