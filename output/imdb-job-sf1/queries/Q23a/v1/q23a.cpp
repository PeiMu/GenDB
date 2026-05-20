// Q23a - complete US internet movies (MIN kt.kind, MIN t.title)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static inline bool memmem_simple(const char* hay, size_t hlen, const char* needle, size_t nlen) {
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    return memmem(hay, hlen, needle, nlen) != nullptr;
}

// Resolve a literal string in a varlen column (.off + .dat).
// Returns the row index (0-based) of the first match, or -1 if none.
static int resolve_varlen(const std::string& off_path, const std::string& dat_path, const char* literal) {
    MmapColumn<int64_t> off(off_path);
    MmapColumn<char> dat(dat_path);
    size_t n = off.count - 1;
    size_t lit_len = strlen(literal);
    for (size_t i = 0; i < n; ++i) {
        int64_t lo = off[i], hi = off[i+1];
        size_t len = (size_t)(hi - lo);
        if (len == lit_len && memcmp(dat.data + lo, literal, lit_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];

    // Ensure results dir exists
    mkdir(results.c_str(), 0755);

    // ----- Dim resolution -----
    int32_t cct1_id = 0, it1_id = 0, kt_id = 0;
    int16_t us_code = 0;
    std::string kt_kind_str;

    {
        GENDB_PHASE("dim_resolve");

        // comp_cast_type: kind = 'complete+verified'
        {
            int row = resolve_varlen(gendb + "/comp_cast_type/kind.off",
                                     gendb + "/comp_cast_type/kind.dat",
                                     "complete+verified");
            if (row < 0) { std::fprintf(stderr, "cct1 not found\n"); return 2; }
            MmapColumn<int32_t> ids(gendb + "/comp_cast_type/id.bin");
            cct1_id = ids[row];
        }
        // info_type: info = 'release dates'
        {
            int row = resolve_varlen(gendb + "/info_type/info.off",
                                     gendb + "/info_type/info.dat",
                                     "release dates");
            if (row < 0) { std::fprintf(stderr, "it1 not found\n"); return 2; }
            MmapColumn<int32_t> ids(gendb + "/info_type/id.bin");
            it1_id = ids[row];
        }
        // kind_type: kind = 'movie'
        {
            int row = resolve_varlen(gendb + "/kind_type/kind.off",
                                     gendb + "/kind_type/kind.dat",
                                     "movie");
            if (row < 0) { std::fprintf(stderr, "kt 'movie' not found\n"); return 2; }
            MmapColumn<int32_t> ids(gendb + "/kind_type/id.bin");
            kt_id = ids[row];
            kt_kind_str = "movie";
        }
        // company_name.country_code dict: resolve '[us]'
        {
            MmapColumn<int64_t> doff(gendb + "/company_name/country_code.dict.off");
            MmapColumn<char> ddat(gendb + "/company_name/country_code.dict.dat");
            size_t K = doff.count - 1;
            const char* lit = "[us]";
            size_t lit_len = 4;
            int found = -1;
            for (size_t i = 0; i < K; ++i) {
                int64_t lo = doff[i], hi = doff[i+1];
                size_t len = (size_t)(hi - lo);
                if (len == lit_len && memcmp(ddat.data + lo, lit, lit_len) == 0) {
                    found = (int)i;
                    break;
                }
            }
            if (found < 0) { std::fprintf(stderr, "country_code '[us]' not found in dict\n"); return 2; }
            // code i references dict entry i-1, so dict index `found` = code (found+1)
            us_code = (int16_t)(found + 1);
        }
    }

    // ----- Open all data files -----
    MmapColumn<int32_t> t_kind_id, t_prod_year;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char> t_title_dat;

    MmapColumn<int32_t> cc_off, mk_off, mc_off, mi_off;
    MmapColumn<int32_t> cc_status_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int16_t> cn_country_code;
    MmapColumn<int32_t> mi_info_type_id;
    MmapColumn<int64_t> mi_info_off, mi_note_off;
    MmapColumn<char> mi_info_dat, mi_note_dat;

    {
        GENDB_PHASE("data_loading");
        t_kind_id.open(gendb + "/title/kind_id.bin");
        t_prod_year.open(gendb + "/title/production_year.bin");
        t_title_off.open(gendb + "/title/title.off");
        t_title_dat.open(gendb + "/title/title.dat");

        cc_off.open(gendb + "/_idx/complete_cast__movie_id__offsets.bin");
        mk_off.open(gendb + "/_idx/movie_keyword__movie_id__offsets.bin");
        mc_off.open(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
        mi_off.open(gendb + "/_idx/movie_info__movie_id__offsets.bin");

        cc_status_id.open(gendb + "/complete_cast/status_id.bin");
        mc_company_id.open(gendb + "/movie_companies/company_id.bin");
        cn_country_code.open(gendb + "/company_name/country_code.bin");
        cn_country_code.advise_random();

        mi_info_type_id.open(gendb + "/movie_info/info_type_id.bin");
        mi_info_off.open(gendb + "/movie_info/info.off");
        mi_note_off.open(gendb + "/movie_info/note.off");
        mi_info_dat.open(gendb + "/movie_info/info.dat");
        mi_note_dat.open(gendb + "/movie_info/note.dat");

        mmap_prefetch_all(t_kind_id, t_prod_year);
        cc_off.prefetch(); mk_off.prefetch(); mc_off.prefetch(); mi_off.prefetch();
    }

    size_t N_titles = t_kind_id.count;
    // Title id v in 1..N_titles; row index = v-1.

    // ----- Main scan -----
    // Per-thread min title state: track (lo, hi) of the current min title in t_title.
    struct ThreadState {
        bool has;
        int64_t lo;
        int64_t hi;
    };

    int max_threads = omp_get_max_threads();
    std::vector<ThreadState> tstate(max_threads, {false, 0, 0});

    auto cmp_title = [&](int64_t a_lo, int64_t a_hi, int64_t b_lo, int64_t b_hi) -> int {
        // compare title slice [a_lo,a_hi) vs [b_lo,b_hi) lexicographically
        size_t la = (size_t)(a_hi - a_lo);
        size_t lb = (size_t)(b_hi - b_lo);
        size_t m = la < lb ? la : lb;
        int c = memcmp(t_title_dat.data + a_lo, t_title_dat.data + b_lo, m);
        if (c != 0) return c;
        if (la < lb) return -1;
        if (la > lb) return 1;
        return 0;
    };

    {
        GENDB_PHASE("main_scan");

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            ThreadState local = {false, 0, 0};

            const int32_t* tk = t_kind_id.data;
            const int32_t* tp = t_prod_year.data;
            const int64_t* tt_off = t_title_off.data;

            const int32_t* cc_o = cc_off.data;
            const int32_t* mk_o = mk_off.data;
            const int32_t* mc_o = mc_off.data;
            const int32_t* mi_o = mi_off.data;

            const int32_t* cc_status = cc_status_id.data;
            const int32_t* mc_cid    = mc_company_id.data;
            const int16_t* cn_cc     = cn_country_code.data;
            const int32_t* mi_iti    = mi_info_type_id.data;
            const int64_t* mi_io     = mi_info_off.data;
            const int64_t* mi_no     = mi_note_off.data;
            const char* mi_id_dat    = mi_info_dat.data;
            const char* mi_nd_dat    = mi_note_dat.data;

            #pragma omp for schedule(dynamic, 4096) nowait
            for (size_t v = 1; v <= N_titles; ++v) {
                size_t r = v - 1;
                // Title filters
                int32_t py = tp[r];
                if (py == INT32_MIN || py <= 2000) continue;
                if (tk[r] != kt_id) continue;

                // complete_cast probe — status_id == cct1_id
                {
                    int32_t lo = cc_o[v], hi = cc_o[v+1];
                    bool found = false;
                    for (int32_t i = lo; i < hi; ++i) {
                        if (cc_status[i] == cct1_id) { found = true; break; }
                    }
                    if (!found) continue;
                }

                // movie_keyword existence
                {
                    int32_t lo = mk_o[v], hi = mk_o[v+1];
                    if (lo >= hi) continue;
                }

                // movie_companies probe — country_code[company_id-1] == us_code
                {
                    int32_t lo = mc_o[v], hi = mc_o[v+1];
                    bool found = false;
                    for (int32_t i = lo; i < hi; ++i) {
                        int32_t cid = mc_cid[i];
                        if (cid <= 0) continue;
                        if (cn_cc[cid - 1] == us_code) { found = true; break; }
                    }
                    if (!found) continue;
                }

                // movie_info probe — info_type_id == it1_id, note has 'internet',
                //                    info starts with 'USA:' and contains ' 199' or ' 200'
                {
                    int32_t lo = mi_o[v], hi = mi_o[v+1];
                    bool found = false;
                    for (int32_t i = lo; i < hi; ++i) {
                        if (mi_iti[i] != it1_id) continue;
                        // note LIKE '%internet%'  (empty note -> NULL -> fail)
                        int64_t nlo = mi_no[i], nhi = mi_no[i+1];
                        size_t nlen = (size_t)(nhi - nlo);
                        if (nlen == 0) continue;
                        if (!memmem_simple(mi_nd_dat + nlo, nlen, "internet", 8)) continue;
                        // info IS NOT NULL AND LIKE 'USA:%' AND (LIKE ' 199' OR ' 200')
                        int64_t ilo = mi_io[i], ihi = mi_io[i+1];
                        size_t ilen = (size_t)(ihi - ilo);
                        if (ilen < 4) continue;
                        if (memcmp(mi_id_dat + ilo, "USA:", 4) != 0) continue;
                        // rest of info: search for ' 199' or ' 200'
                        const char* p = mi_id_dat + ilo + 4;
                        size_t rest = ilen - 4;
                        if (!memmem_simple(p, rest, " 199", 4) &&
                            !memmem_simple(p, rest, " 200", 4)) continue;
                        found = true;
                        break;
                    }
                    if (!found) continue;
                }

                // Survivor: compare title to local min
                int64_t a_lo = tt_off[r], a_hi = tt_off[r+1];
                if (!local.has) {
                    local.has = true;
                    local.lo = a_lo;
                    local.hi = a_hi;
                } else {
                    // compare candidate (a) vs current local (lo,hi)
                    size_t la = (size_t)(a_hi - a_lo);
                    size_t lb = (size_t)(local.hi - local.lo);
                    size_t m = la < lb ? la : lb;
                    int c = memcmp(t_title_dat.data + a_lo, t_title_dat.data + local.lo, m);
                    bool smaller;
                    if (c != 0) smaller = (c < 0);
                    else smaller = (la < lb);
                    if (smaller) {
                        local.lo = a_lo;
                        local.hi = a_hi;
                    }
                }
            }

            tstate[tid] = local;
        }
    }

    // Reduce
    bool any = false;
    int64_t best_lo = 0, best_hi = 0;
    for (auto& s : tstate) {
        if (!s.has) continue;
        if (!any) { any = true; best_lo = s.lo; best_hi = s.hi; continue; }
        size_t la = (size_t)(s.hi - s.lo);
        size_t lb = (size_t)(best_hi - best_lo);
        size_t m = la < lb ? la : lb;
        int c = memcmp(t_title_dat.data + s.lo, t_title_dat.data + best_lo, m);
        bool smaller;
        if (c != 0) smaller = (c < 0);
        else smaller = (la < lb);
        if (smaller) { best_lo = s.lo; best_hi = s.hi; }
    }

    // ----- Output -----
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q23a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); return 3; }
        std::fprintf(fp, "movie_kind,complete_us_internet_movie\n");
        if (any) {
            std::fprintf(fp, "%s,", kt_kind_str.c_str());
            std::fwrite(t_title_dat.data + best_lo, 1, (size_t)(best_hi - best_lo), fp);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
