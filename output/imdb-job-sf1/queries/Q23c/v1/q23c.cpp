// Q23c: Movies (4 kinds) since 1990 with US release on internet, complete+verified
// Driver: title; per-row existence joins on cc/mc/mk/mi using offsets_only indexes.

#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static std::vector<char> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

template<typename T>
static std::vector<T> read_vec(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(0, std::ios::end);
    size_t bytes = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<T> v(bytes / sizeof(T));
    f.read((char*)v.data(), bytes);
    return v;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb_dir = argv[1];
    std::string results_dir = argv[2];
    mkdir(results_dir.c_str(), 0755);

    // --- Resolved at startup ---
    int32_t cct1_id = -1;
    int32_t it1_id  = -1;
    int32_t us_code = -1; // dict code (int from int16 code domain)
    std::string kt_id_to_kind[8];
    uint8_t kt_mask = 0;

    // Memory-mapped data
    MmapColumn<int32_t> t_kind_id, t_py;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> cc_off, mc_off, mk_off, mi_off;
    MmapColumn<int32_t> cc_status_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int16_t> cn_country_code;

    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off, mi_note_off;
    MmapColumn<char>    mi_info_dat, mi_note_dat;

    {
        GENDB_PHASE("data_loading");

        // --- comp_cast_type: cct1_id where kind == 'complete+verified' ---
        {
            auto ids = read_vec<int32_t>(gendb_dir + "/comp_cast_type/id.bin");
            auto offs = read_vec<int64_t>(gendb_dir + "/comp_cast_type/kind.off");
            auto dat = read_all(gendb_dir + "/comp_cast_type/kind.dat");
            for (size_t i = 0; i < ids.size(); i++) {
                size_t lo = offs[i], hi = offs[i+1];
                std::string s(dat.data()+lo, hi-lo);
                if (s == "complete+verified") { cct1_id = ids[i]; break; }
            }
        }

        // --- info_type: it1_id where info == 'release dates' ---
        {
            auto ids = read_vec<int32_t>(gendb_dir + "/info_type/id.bin");
            auto offs = read_vec<int64_t>(gendb_dir + "/info_type/info.off");
            auto dat = read_all(gendb_dir + "/info_type/info.dat");
            for (size_t i = 0; i < ids.size(); i++) {
                size_t lo = offs[i], hi = offs[i+1];
                std::string s(dat.data()+lo, hi-lo);
                if (s == "release dates") { it1_id = ids[i]; break; }
            }
        }

        // --- kind_type: kt_ids + id->kind map ---
        {
            auto ids = read_vec<int32_t>(gendb_dir + "/kind_type/id.bin");
            auto offs = read_vec<int64_t>(gendb_dir + "/kind_type/kind.off");
            auto dat = read_all(gendb_dir + "/kind_type/kind.dat");
            std::unordered_set<std::string> targets = {
                "movie", "tv movie", "video movie", "video game"
            };
            for (size_t i = 0; i < ids.size(); i++) {
                size_t lo = offs[i], hi = offs[i+1];
                std::string s(dat.data()+lo, hi-lo);
                int32_t id = ids[i];
                if (id >= 0 && id < 8) kt_id_to_kind[id] = s;
                if (targets.count(s) && id >= 0 && id < 8) {
                    kt_mask |= (uint8_t)(1u << id);
                }
            }
        }

        // --- company_name country_code dict: resolve us_code ---
        {
            auto offs = read_vec<int64_t>(gendb_dir + "/company_name/country_code.dict.off");
            auto dat  = read_all(gendb_dir + "/company_name/country_code.dict.dat");
            // K+1 offsets => K dict entries; code 0 = NULL, code i references entry i-1
            for (size_t i = 0; i + 1 < offs.size(); i++) {
                size_t lo = offs[i], hi = offs[i+1];
                std::string s(dat.data()+lo, hi-lo);
                if (s == "[us]") { us_code = (int32_t)(i + 1); break; }
            }
        }

        if (cct1_id < 0 || it1_id < 0 || us_code < 0 || kt_mask == 0) {
            std::fprintf(stderr, "Failed to resolve dim ids: cct1=%d it1=%d us=%d kt_mask=%u\n",
                         cct1_id, it1_id, us_code, (unsigned)kt_mask);
            return 1;
        }

        // --- Open mmap columns ---
        t_kind_id.open(gendb_dir + "/title/kind_id.bin");
        t_py.open(gendb_dir + "/title/production_year.bin");
        t_title_off.open(gendb_dir + "/title/title.off");
        t_title_dat.open(gendb_dir + "/title/title.dat");

        cc_off.open(gendb_dir + "/_idx/complete_cast__movie_id__offsets.bin");
        mc_off.open(gendb_dir + "/_idx/movie_companies__movie_id__offsets.bin");
        mk_off.open(gendb_dir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mi_off.open(gendb_dir + "/_idx/movie_info__movie_id__offsets.bin");

        cc_status_id.open(gendb_dir + "/complete_cast/status_id.bin");
        mc_company_id.open(gendb_dir + "/movie_companies/company_id.bin");
        cn_country_code.open(gendb_dir + "/company_name/country_code.bin");

        mi_info_type_id.open(gendb_dir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb_dir + "/movie_info/info.off");
        mi_info_dat.open(gendb_dir + "/movie_info/info.dat");
        mi_note_off.open(gendb_dir + "/movie_info/note.off");
        mi_note_dat.open(gendb_dir + "/movie_info/note.dat");

        // Country code (small) — random access during scan
        cn_country_code.advise_random();
        // Offsets — random access (per-movie lookups)
        cc_off.advise_random();
        mc_off.advise_random();
        mk_off.advise_random();
        mi_off.advise_random();

        mmap_prefetch_all(t_kind_id, t_py, cc_status_id, mc_company_id, mi_info_type_id);
    }

    // --- Main scan parallelized ---
    size_t N = t_kind_id.count;
    int16_t us_code16 = (int16_t)us_code;

    struct LocalMin {
        uint8_t seen_kt_mask = 0;
        const char* title_ptr = nullptr;
        size_t title_len = 0;
        char _pad[40]; // false-sharing pad
    };

    auto title_lt = [](const char* a, size_t la, const char* b, size_t lb) -> bool {
        size_t m = (la < lb) ? la : lb;
        int c = std::memcmp(a, b, m);
        if (c != 0) return c < 0;
        return la < lb;
    };

    int max_threads = omp_get_max_threads();
    std::vector<LocalMin> locals(max_threads);

    {
        GENDB_PHASE("main_scan");

        const int32_t* kind_id_arr = t_kind_id.data;
        const int32_t* py_arr      = t_py.data;
        const int64_t* title_off_arr = t_title_off.data;
        const char*    title_dat_arr = t_title_dat.data;

        const int32_t* cc_off_arr = cc_off.data;
        const int32_t* mc_off_arr = mc_off.data;
        const int32_t* mk_off_arr = mk_off.data;
        const int32_t* mi_off_arr = mi_off.data;

        const int32_t* cc_status_arr = cc_status_id.data;
        const int32_t* mc_company_arr = mc_company_id.data;
        const int16_t* cn_cc_arr = cn_country_code.data;

        const int32_t* mi_iti_arr      = mi_info_type_id.data;
        const int64_t* mi_info_off_arr = mi_info_off.data;
        const char*    mi_info_dat_arr = mi_info_dat.data;
        const int64_t* mi_note_off_arr = mi_note_off.data;
        const char*    mi_note_dat_arr = mi_note_dat.data;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            LocalMin lm;

            #pragma omp for schedule(static)
            for (size_t v = 1; v <= N; v++) {
                size_t idx = v - 1;
                int32_t kid = kind_id_arr[idx];
                if ((unsigned)kid >= 8u) continue;
                if (!((kt_mask >> kid) & 1u)) continue;
                int32_t py = py_arr[idx];
                if (py <= 1990) continue; // INT32_MIN naturally excluded

                // Step 2: complete_cast existence with status_id == cct1_id
                int32_t lo = cc_off_arr[v], hi = cc_off_arr[v+1];
                bool ok = false;
                for (int32_t r = lo; r < hi; r++) {
                    if (cc_status_arr[r] == cct1_id) { ok = true; break; }
                }
                if (!ok) continue;

                // Step 3: movie_companies existence with country_code[company_id-1] == us_code
                lo = mc_off_arr[v]; hi = mc_off_arr[v+1];
                ok = false;
                for (int32_t r = lo; r < hi; r++) {
                    int32_t cid = mc_company_arr[r];
                    if (cid >= 1 && cn_cc_arr[cid-1] == us_code16) { ok = true; break; }
                }
                if (!ok) continue;

                // Step 4: movie_keyword existence (any)
                lo = mk_off_arr[v]; hi = mk_off_arr[v+1];
                if (lo >= hi) continue;

                // Step 5: movie_info filter
                lo = mi_off_arr[v]; hi = mi_off_arr[v+1];
                ok = false;
                for (int32_t r = lo; r < hi; r++) {
                    if (mi_iti_arr[r] != it1_id) continue;
                    int64_t no_lo = mi_note_off_arr[r], no_hi = mi_note_off_arr[r+1];
                    size_t nlen = (size_t)(no_hi - no_lo);
                    if (nlen < 8) continue;
                    if (!memmem(mi_note_dat_arr + no_lo, nlen, "internet", 8)) continue;
                    int64_t io_lo = mi_info_off_arr[r], io_hi = mi_info_off_arr[r+1];
                    size_t ilen = (size_t)(io_hi - io_lo);
                    if (ilen < 4) continue;
                    const char* idat = mi_info_dat_arr + io_lo;
                    if (std::memcmp(idat, "USA:", 4) != 0) continue;
                    if (!memmem(idat, ilen, " 199", 4) && !memmem(idat, ilen, " 200", 4)) continue;
                    ok = true;
                    break;
                }
                if (!ok) continue;

                // Step 6: update local min
                lm.seen_kt_mask |= (uint8_t)(1u << kid);
                int64_t to_lo = title_off_arr[idx], to_hi = title_off_arr[idx+1];
                const char* tptr = title_dat_arr + to_lo;
                size_t tlen = (size_t)(to_hi - to_lo);
                if (lm.title_ptr == nullptr || title_lt(tptr, tlen, lm.title_ptr, lm.title_len)) {
                    lm.title_ptr = tptr;
                    lm.title_len = tlen;
                }
            }

            locals[tid] = lm;
        }
    }

    // --- Reduce ---
    uint8_t global_kt_mask = 0;
    const char* best_title = nullptr;
    size_t best_title_len = 0;
    for (auto& lm : locals) {
        global_kt_mask |= lm.seen_kt_mask;
        if (lm.title_ptr) {
            if (best_title == nullptr || title_lt(lm.title_ptr, lm.title_len, best_title, best_title_len)) {
                best_title = lm.title_ptr;
                best_title_len = lm.title_len;
            }
        }
    }

    std::string best_kind;
    bool first = true;
    for (int id = 0; id < 8; id++) {
        if (!((global_kt_mask >> id) & 1u)) continue;
        if (first || kt_id_to_kind[id] < best_kind) {
            best_kind = kt_id_to_kind[id];
            first = false;
        }
    }

    // --- Output CSV ---
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q23c.csv";
        FILE* f = std::fopen(out_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Cannot write %s\n", out_path.c_str());
            return 1;
        }
        std::fprintf(f, "movie_kind,complete_us_internet_movie\n");
        if (best_title) {
            std::fwrite(best_kind.data(), 1, best_kind.size(), f);
            std::fputc(',', f);
            std::fwrite(best_title, 1, best_title_len, f);
            std::fputc('\n', f);
        } else {
            std::fprintf(f, ",\n");
        }
        std::fclose(f);
    }

    return 0;
}
