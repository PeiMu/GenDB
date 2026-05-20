// Q15c — MIN(mi.info), MIN(t.title) across release_dates/internet/USA 199-200/year>1990/[us]
// Driver: movie_info CSR on info_type_id == 'release dates'
#define _GNU_SOURCE
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static const char* g_storage;

static inline int compare_sv(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    int c = std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c;
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    g_storage = store.c_str();

    // ---------------- data_loading ----------------
    MmapColumn<int64_t> it_off;
    MmapColumn<char>    it_dat;

    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_dict_off;
    MmapColumn<char>    cn_dict_dat;

    MmapColumn<int32_t> mi_it_csr_off;
    MmapColumn<int32_t> mi_it_csr_row;
    MmapColumn<int32_t> mi_movie_id;
    MmapColumn<int64_t> mi_info_off;
    MmapColumn<char>    mi_info_dat;
    MmapColumn<int64_t> mi_note_off;
    MmapColumn<char>    mi_note_dat;

    MmapColumn<int32_t> t_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mc_off;       // offsets_only by movie_id (mc sorted by movie_id)
    MmapColumn<int32_t> mc_company_id;

    MmapColumn<int32_t> at_off;
    MmapColumn<int32_t> mk_off;

    {
        GENDB_PHASE("data_loading");
        it_off.open(store + "/info_type/info.off");
        it_dat.open(store + "/info_type/info.dat");

        cn_cc.open(store + "/company_name/country_code.bin");
        cn_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_dict_dat.open(store + "/company_name/country_code.dict.dat");

        mi_it_csr_off.open(store + "/_idx/movie_info__info_type_id__offsets.bin");
        mi_it_csr_row.open(store + "/_idx/movie_info__info_type_id__rowids.bin");
        mi_movie_id.open(store + "/movie_info/movie_id.bin");
        mi_info_off.open(store + "/movie_info/info.off");
        mi_info_dat.open(store + "/movie_info/info.dat");
        mi_note_off.open(store + "/movie_info/note.off");
        mi_note_dat.open(store + "/movie_info/note.dat");

        t_year.open(store + "/title/production_year.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        mc_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        mc_company_id.open(store + "/movie_companies/company_id.bin");

        at_off.open(store + "/_idx/aka_title__movie_id__offsets.bin");
        mk_off.open(store + "/_idx/movie_keyword__movie_id__offsets.bin");

        // Prefetch hot data we'll random-access (small + medium tables)
        mi_movie_id.advise_random();
        mi_info_off.advise_random();
        mi_info_dat.advise_random();
        mi_note_off.advise_random();
        mi_note_dat.advise_random();
        t_year.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
        mc_off.advise_random();
        mc_company_id.advise_random();
        at_off.advise_random();
        mk_off.advise_random();
        cn_cc.advise_random();
    }

    // ---------------- resolve_dict_codes ----------------
    int32_t it1_id = -1;
    {
        size_t n_it = it_off.size() - 1;
        static const char* target = "release dates";
        size_t tlen = std::strlen(target);
        MmapColumn<int32_t> it_id(store + "/info_type/id.bin");
        for (size_t i = 0; i < n_it; ++i) {
            int64_t a = it_off[i], b = it_off[i + 1];
            if ((size_t)(b - a) == tlen && std::memcmp(it_dat.data + a, target, tlen) == 0) {
                it1_id = it_id[i];
                break;
            }
        }
    }
    if (it1_id < 0) {
        std::fprintf(stderr, "Could not find info_type 'release dates'\n");
        return 1;
    }

    int16_t us_code = -1;
    {
        // Find dict entry == "[us]" in cn_dict
        size_t n_codes = cn_dict_off.size() - 1;
        static const char* target = "[us]";
        size_t tlen = std::strlen(target);
        for (size_t i = 0; i < n_codes; ++i) {
            int64_t a = cn_dict_off[i], b = cn_dict_off[i + 1];
            if ((size_t)(b - a) == tlen && std::memcmp(cn_dict_dat.data + a, target, tlen) == 0) {
                us_code = (int16_t)(i + 1); // 1-based encoding (0 = NULL)
                break;
            }
        }
    }
    if (us_code < 0) {
        std::fprintf(stderr, "Could not find country_code '[us]'\n");
        return 1;
    }

    // ---------------- main_scan ----------------
    // Parallel scan over mi_it_csr_row[lo..hi)
    int32_t lo = mi_it_csr_off[(size_t)it1_id];
    int32_t hi = mi_it_csr_off[(size_t)it1_id + 1];

    // Per-thread MIN trackers
    struct MinPair {
        std::string_view mi_info{};
        std::string_view t_title{};
        bool have = false;
    };

    int nthreads = std::max(1u, std::thread::hardware_concurrency());
    if (nthreads > 12) nthreads = 12;
    std::vector<MinPair> locals(nthreads);

    auto worker = [&](int tid, int32_t a, int32_t b) {
        std::string_view best_info{};
        std::string_view best_title{};
        bool have = false;

        const int32_t* rows = mi_it_csr_row.data;
        const int32_t* mid_arr = mi_movie_id.data;
        const int64_t* info_o = mi_info_off.data;
        const char*    info_d = mi_info_dat.data;
        const int64_t* note_o = mi_note_off.data;
        const char*    note_d = mi_note_dat.data;
        const int32_t* yr = t_year.data;
        const int64_t* title_o = t_title_off.data;
        const char*    title_d = t_title_dat.data;
        const int32_t* mc_o = mc_off.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int16_t* cn_country = cn_cc.data;
        const int32_t* at_o = at_off.data;
        const int32_t* mk_o = mk_off.data;
        int16_t us = us_code;

        for (int32_t k = a; k < b; ++k) {
            int32_t r = rows[k];

            // 1) note LIKE '%internet%'
            int64_t na = note_o[r], nb = note_o[r + 1];
            size_t nlen = (size_t)(nb - na);
            if (nlen < 8) continue;
            if (memmem(note_d + na, nlen, "internet", 8) == nullptr) continue;

            // 2) info LIKE 'USA:% 199%' OR 'USA:% 200%'
            int64_t ia = info_o[r], ib = info_o[r + 1];
            size_t ilen = (size_t)(ib - ia);
            if (ilen < 8) continue;
            const char* ip = info_d + ia;
            if (std::memcmp(ip, "USA:", 4) != 0) continue;
            const void* m1 = memmem(ip + 4, ilen - 4, " 199", 4);
            const void* m2 = m1 ? nullptr : memmem(ip + 4, ilen - 4, " 200", 4);
            if (!m1 && !m2) continue;

            // 3) production_year > 1990
            int32_t mv = mid_arr[r];
            if (mv <= 0) continue;
            int32_t py = yr[(size_t)mv - 1];
            if (py == INT32_MIN || py <= 1990) continue;

            // 4) mc semi-join: some mc row with cn.country_code == us
            int32_t ma = mc_o[(size_t)mv];
            int32_t mb = mc_o[(size_t)mv + 1];
            bool mc_ok = false;
            for (int32_t j = ma; j < mb; ++j) {
                int32_t cid = mc_cid[j];
                if (cid > 0 && cn_country[(size_t)cid - 1] == us) {
                    mc_ok = true;
                    break;
                }
            }
            if (!mc_ok) continue;

            // 5) at1 existence
            if (at_o[(size_t)mv + 1] <= at_o[(size_t)mv]) continue;
            // 6) mk existence
            if (mk_o[(size_t)mv + 1] <= mk_o[(size_t)mv]) continue;

            // Passed all filters — update MINs
            std::string_view cur_info(info_d + ia, ilen);
            std::string_view cur_title(title_d + title_o[(size_t)mv - 1],
                                       (size_t)(title_o[(size_t)mv] - title_o[(size_t)mv - 1]));
            if (!have) {
                best_info = cur_info;
                best_title = cur_title;
                have = true;
            } else {
                if (compare_sv(cur_info, best_info) < 0) best_info = cur_info;
                if (compare_sv(cur_title, best_title) < 0) best_title = cur_title;
            }
        }

        locals[tid].mi_info = best_info;
        locals[tid].t_title = best_title;
        locals[tid].have = have;
    };

    {
        GENDB_PHASE("main_scan");
        int32_t total = hi - lo;
        if (total <= 0) {
            // nothing
        } else {
            int32_t chunk = (total + nthreads - 1) / nthreads;
            std::vector<std::thread> ths;
            for (int t = 0; t < nthreads; ++t) {
                int32_t a = lo + t * chunk;
                int32_t b = std::min(hi, a + chunk);
                if (a >= b) continue;
                ths.emplace_back(worker, t, a, b);
            }
            for (auto& th : ths) th.join();
        }
    }

    // Reduce
    std::string_view final_info{};
    std::string_view final_title{};
    bool have_any = false;
    for (auto& l : locals) {
        if (!l.have) continue;
        if (!have_any) {
            final_info = l.mi_info;
            final_title = l.t_title;
            have_any = true;
        } else {
            if (compare_sv(l.mi_info, final_info) < 0) final_info = l.mi_info;
            if (compare_sv(l.t_title, final_title) < 0) final_title = l.t_title;
        }
    }

    // ---------------- output ----------------
    {
        GENDB_PHASE("output");
        // Ensure dir exists
        std::string mk = "mkdir -p '" + results_dir + "'";
        (void)std::system(mk.c_str());
        std::string path = results_dir + "/Q15c.csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot open %s\n", path.c_str());
            return 1;
        }
        std::fprintf(f, "release_date,modern_american_internet_movie\n");
        if (have_any) {
            std::fwrite(final_info.data(), 1, final_info.size(), f);
            std::fputc(',', f);
            std::fwrite(final_title.data(), 1, final_title.size(), f);
            std::fputc('\n', f);
        } else {
            std::fprintf(f, ",\n");
        }
        std::fclose(f);
    }

    return 0;
}
