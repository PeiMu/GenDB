// Q15b implementation
#define _GNU_SOURCE
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <climits>
#include <algorithm>

#include "timing_utils.h"

struct MMapFile {
    void* ptr = nullptr;
    size_t size = 0;
    int fd = -1;
    ~MMapFile() {
        if (ptr && ptr != MAP_FAILED) munmap(ptr, size);
        if (fd >= 0) close(fd);
    }
};

static bool mmap_file(const std::string& path, MMapFile& out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open failed: %s\n", path.c_str()); return false; }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return false; }
    size_t sz = (size_t)st.st_size;
    void* p = nullptr;
    if (sz > 0) {
        p = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { close(fd); return false; }
    }
    out.fd = fd; out.ptr = p; out.size = sz;
    return true;
}

template<typename T>
struct ColView {
    const T* data = nullptr;
    size_t count = 0;
};

template<typename T>
static ColView<T> as_col(const MMapFile& f) {
    ColView<T> v;
    v.data = (const T*)f.ptr;
    v.count = f.size / sizeof(T);
    return v;
}

static inline const void* my_memmem(const void* hay, size_t hl, const void* nd, size_t nl) {
    return memmem(hay, hl, nd, nl);
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) { fprintf(stderr, "Usage: q15b <gendb_dir> <results_dir>\n"); return 1; }
    std::string store = argv[1];
    std::string results_dir = argv[2];

    // mmap all needed files
    MMapFile f_cc_dict_off, f_cc_dict_dat, f_cn_cc, f_cn_name_off, f_cn_name_dat;
    MMapFile f_it_info_off, f_it_info_dat;
    MMapFile f_mc_co_off, f_mc_co_row;
    MMapFile f_mc_note_off, f_mc_note_dat, f_mc_movie_id;
    MMapFile f_t_py, f_t_title_off, f_t_title_dat;
    MMapFile f_aka_off, f_mk_off, f_mi_off;
    MMapFile f_mi_info_off, f_mi_info_dat, f_mi_note_off, f_mi_note_dat, f_mi_itid, f_mi_movie_id;

    {
        GENDB_PHASE("data_loading");
        if (!mmap_file(store + "/company_name/country_code.dict.off", f_cc_dict_off)) return 2;
        if (!mmap_file(store + "/company_name/country_code.dict.dat", f_cc_dict_dat)) return 2;
        if (!mmap_file(store + "/company_name/country_code.bin", f_cn_cc)) return 2;
        if (!mmap_file(store + "/company_name/name.off", f_cn_name_off)) return 2;
        if (!mmap_file(store + "/company_name/name.dat", f_cn_name_dat)) return 2;

        if (!mmap_file(store + "/info_type/info.off", f_it_info_off)) return 2;
        if (!mmap_file(store + "/info_type/info.dat", f_it_info_dat)) return 2;

        if (!mmap_file(store + "/_idx/movie_companies__company_id__offsets.bin", f_mc_co_off)) return 2;
        if (!mmap_file(store + "/_idx/movie_companies__company_id__rowids.bin", f_mc_co_row)) return 2;
        if (!mmap_file(store + "/movie_companies/note.off", f_mc_note_off)) return 2;
        if (!mmap_file(store + "/movie_companies/note.dat", f_mc_note_dat)) return 2;
        if (!mmap_file(store + "/movie_companies/movie_id.bin", f_mc_movie_id)) return 2;

        if (!mmap_file(store + "/title/production_year.bin", f_t_py)) return 2;
        if (!mmap_file(store + "/title/title.off", f_t_title_off)) return 2;
        if (!mmap_file(store + "/title/title.dat", f_t_title_dat)) return 2;

        if (!mmap_file(store + "/_idx/aka_title__movie_id__offsets.bin", f_aka_off)) return 2;
        if (!mmap_file(store + "/_idx/movie_keyword__movie_id__offsets.bin", f_mk_off)) return 2;
        if (!mmap_file(store + "/_idx/movie_info__movie_id__offsets.bin", f_mi_off)) return 2;

        if (!mmap_file(store + "/movie_info/info.off", f_mi_info_off)) return 2;
        if (!mmap_file(store + "/movie_info/info.dat", f_mi_info_dat)) return 2;
        if (!mmap_file(store + "/movie_info/note.off", f_mi_note_off)) return 2;
        if (!mmap_file(store + "/movie_info/note.dat", f_mi_note_dat)) return 2;
        if (!mmap_file(store + "/movie_info/info_type_id.bin", f_mi_itid)) return 2;
        if (!mmap_file(store + "/movie_info/movie_id.bin", f_mi_movie_id)) return 2;
    }

    // Column views
    auto cc_dict_off = as_col<int64_t>(f_cc_dict_off);
    const char* cc_dict_dat = (const char*)f_cc_dict_dat.ptr;
    auto cn_cc = as_col<int16_t>(f_cn_cc);
    auto cn_name_off = as_col<int64_t>(f_cn_name_off);
    const char* cn_name_dat = (const char*)f_cn_name_dat.ptr;

    auto it_info_off = as_col<int64_t>(f_it_info_off);
    const char* it_info_dat = (const char*)f_it_info_dat.ptr;

    auto mc_co_off = as_col<int32_t>(f_mc_co_off);
    auto mc_co_row = as_col<int32_t>(f_mc_co_row);
    auto mc_note_off = as_col<int64_t>(f_mc_note_off);
    const char* mc_note_dat = (const char*)f_mc_note_dat.ptr;
    auto mc_movie_id = as_col<int32_t>(f_mc_movie_id);

    auto t_py = as_col<int32_t>(f_t_py);
    auto t_title_off = as_col<int64_t>(f_t_title_off);
    const char* t_title_dat = (const char*)f_t_title_dat.ptr;

    auto aka_off = as_col<int32_t>(f_aka_off);
    auto mk_off = as_col<int32_t>(f_mk_off);
    auto mi_off = as_col<int32_t>(f_mi_off);

    auto mi_info_off = as_col<int64_t>(f_mi_info_off);
    const char* mi_info_dat = (const char*)f_mi_info_dat.ptr;
    auto mi_note_off = as_col<int64_t>(f_mi_note_off);
    const char* mi_note_dat = (const char*)f_mi_note_dat.ptr;
    auto mi_itid = as_col<int32_t>(f_mi_itid);
    (void)mi_itid;
    auto mi_movie_id_col = as_col<int32_t>(f_mi_movie_id);
    (void)mi_movie_id_col;

    // Resolve us_code (1-based: 0 means NULL)
    int16_t us_code = 0;
    {
        size_t n = cc_dict_off.count > 0 ? cc_dict_off.count - 1 : 0;
        for (size_t i = 0; i < n; ++i) {
            int64_t a = cc_dict_off.data[i], b = cc_dict_off.data[i+1];
            if (b - a == 4 && memcmp(cc_dict_dat + a, "[us]", 4) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }
    }

    // Resolve cn_ids for name='YouTube'
    std::vector<int32_t> cn_ids;
    {
        size_t n = cn_name_off.count > 0 ? cn_name_off.count - 1 : 0;
        for (size_t i = 0; i < n; ++i) {
            if (cn_cc.data[i] != us_code) continue;
            int64_t a = cn_name_off.data[i], b = cn_name_off.data[i+1];
            if (b - a == 7 && memcmp(cn_name_dat + a, "YouTube", 7) == 0) {
                cn_ids.push_back((int32_t)(i + 1));  // 1-based id for CSR
            }
        }
    }

    // Resolve it1_id (1-based)
    int32_t it1_id = 0;
    {
        size_t n = it_info_off.count > 0 ? it_info_off.count - 1 : 0;
        for (size_t i = 0; i < n; ++i) {
            int64_t a = it_info_off.data[i], b = it_info_off.data[i+1];
            if (b - a == 13 && memcmp(it_info_dat + a, "release dates", 13) == 0) {
                it1_id = (int32_t)(i + 1);
                break;
            }
        }
    }

    // Min trackers: (off, len) into respective .dat buffers
    int64_t best_title_off = -1; int64_t best_title_len = 0;
    int64_t best_mi_off = -1;    int64_t best_mi_len = 0;

    auto cmp_min = [](const char* dat, int64_t cur_off, int64_t cur_len,
                      const char* new_p, int64_t new_len) -> int {
        // returns negative if new_p < current (need update), 0 if equal, positive otherwise
        if (cur_off < 0) return -1;
        int64_t mn = std::min(cur_len, new_len);
        int c = memcmp(new_p, dat + cur_off, (size_t)mn);
        if (c != 0) return c;
        if (new_len < cur_len) return -1;
        if (new_len > cur_len) return 1;
        return 0;
    };

    size_t title_rows = t_py.count;

    {
        GENDB_PHASE("main_scan");
        for (int32_t cn_id : cn_ids) {
            // cn_id is 0-based row index; CSR indexed by cn_id (assume directly)
            // mc_co_off has length 234999 = 234997 rows + 1? Actually 234999 per guide. Let's use cn_id as index.
            if ((size_t)cn_id + 1 >= mc_co_off.count) continue;
            int32_t lo = mc_co_off.data[cn_id];
            int32_t hi = mc_co_off.data[cn_id + 1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = mc_co_row.data[k];
                // Check mc.note LIKE '%(200%)%' AND '%(worldwide)%'
                int64_t na = mc_note_off.data[r], nb = mc_note_off.data[r+1];
                int64_t nlen = nb - na;
                if (nlen <= 0) continue;
                const char* np = mc_note_dat + na;
                // '%(200%)%' means contains '(200' AND then ')' after - approximated as contains '(200' and contains ')'
                // From plan: memmem '(200' AND memmem '(worldwide)'
                if (!my_memmem(np, (size_t)nlen, "(200", 4)) continue;
                if (!my_memmem(np, (size_t)nlen, "(worldwide)", 11)) continue;

                int32_t mv = mc_movie_id.data[r];  // movie_id (1-based id)
                // title row index = mv - 1
                int32_t tr = mv - 1;
                if (tr < 0 || (size_t)tr >= title_rows) continue;
                int32_t py = t_py.data[tr];
                if (py == INT32_MIN) continue;
                if (py < 2005 || py > 2010) continue;

                // aka_title existence via aka_off[mv] < aka_off[mv+1]
                if ((size_t)mv + 1 >= aka_off.count) continue;
                if (aka_off.data[mv] >= aka_off.data[mv + 1]) continue;
                // movie_keyword existence
                if ((size_t)mv + 1 >= mk_off.count) continue;
                if (mk_off.data[mv] >= mk_off.data[mv + 1]) continue;

                // Walk movie_info
                if ((size_t)mv + 1 >= mi_off.count) continue;
                int32_t milo = mi_off.data[mv];
                int32_t mihi = mi_off.data[mv + 1];
                bool found_mi = false;
                int64_t local_best_mi_off = -1, local_best_mi_len = 0;
                for (int32_t j = milo; j < mihi; ++j) {
                    if (mi_itid.data[j] != it1_id) continue;
                    // mi.note LIKE '%internet%'
                    int64_t mna = mi_note_off.data[j], mnb = mi_note_off.data[j+1];
                    int64_t mnl = mnb - mna;
                    if (mnl <= 0) continue;
                    if (!my_memmem(mi_note_dat + mna, (size_t)mnl, "internet", 8)) continue;
                    // mi.info LIKE 'USA:% 200%'
                    int64_t mia = mi_info_off.data[j], mib = mi_info_off.data[j+1];
                    int64_t mil = mib - mia;
                    if (mil < 4) continue;
                    const char* ip = mi_info_dat + mia;
                    if (memcmp(ip, "USA:", 4) != 0) continue;
                    if (!my_memmem(ip + 4, (size_t)(mil - 4), " 200", 4)) continue;
                    found_mi = true;
                    // Track min for this row (across mi rows for same mv)
                    if (local_best_mi_off < 0) {
                        local_best_mi_off = mia;
                        local_best_mi_len = mil;
                    } else {
                        int64_t mn = std::min(local_best_mi_len, mil);
                        int c = memcmp(ip, mi_info_dat + local_best_mi_off, (size_t)mn);
                        if (c < 0 || (c == 0 && mil < local_best_mi_len)) {
                            local_best_mi_off = mia;
                            local_best_mi_len = mil;
                        }
                    }
                }
                if (!found_mi) continue;

                // Update global min on mi.info
                {
                    const char* p = mi_info_dat + local_best_mi_off;
                    int sgn = cmp_min(mi_info_dat, best_mi_off, best_mi_len, p, local_best_mi_len);
                    if (sgn < 0) {
                        best_mi_off = local_best_mi_off;
                        best_mi_len = local_best_mi_len;
                    }
                }
                // Update global min on t.title
                {
                    int64_t ta = t_title_off.data[tr], tb = t_title_off.data[tr+1];
                    int64_t tl = tb - ta;
                    const char* p = t_title_dat + ta;
                    int sgn = cmp_min(t_title_dat, best_title_off, best_title_len, p, tl);
                    if (sgn < 0) {
                        best_title_off = ta;
                        best_title_len = tl;
                    }
                }
            }
        }
    }

    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q15b.csv";
        // Ensure dir exists (orchestrator likely creates it)
        FILE* fp = fopen(out_path.c_str(), "w");
        if (!fp) {
            // Try mkdir
            std::string cmd = "mkdir -p '" + results_dir + "'";
            system(cmd.c_str());
            fp = fopen(out_path.c_str(), "w");
            if (!fp) { fprintf(stderr, "Cannot open %s\n", out_path.c_str()); return 3; }
        }
        fprintf(fp, "release_date,youtube_movie\n");
        if (best_mi_off >= 0 && best_title_off >= 0) {
            // mi.info
            fwrite(mi_info_dat + best_mi_off, 1, (size_t)best_mi_len, fp);
            fputc(',', fp);
            fwrite(t_title_dat + best_title_off, 1, (size_t)best_title_len, fp);
            fputc('\n', fp);
        } else {
            fprintf(fp, ",\n");
        }
        fclose(fp);
    }

    return 0;
}
