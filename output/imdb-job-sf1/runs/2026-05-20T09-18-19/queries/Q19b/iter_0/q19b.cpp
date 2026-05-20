// Q19b — voicing actresses in Kung Fu Panda (2007/2008)
// Strategy: drive on title (very selective LIKE filter); semi-join mc/mi/ci via
// offsets_only indexes; final aggregate is MIN(n.name), MIN(t.title).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline const char* mem_find(const char* hay, size_t hlen,
                                   const char* needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return nullptr;
    return static_cast<const char*>(memmem(hay, hlen, needle, nlen));
}

// Does s contain a, then b, then c, in order (substring sequence)?
static inline bool like_three_in_order(const char* s, size_t slen,
                                       const char* a, size_t alen,
                                       const char* b, size_t blen,
                                       const char* c, size_t clen) {
    const char* p = mem_find(s, slen, a, alen);
    if (!p) return false;
    p += alen;
    size_t rem = slen - (p - s);
    const char* q = mem_find(p, rem, b, blen);
    if (!q) return false;
    q += blen;
    rem = slen - (q - s);
    const char* r = mem_find(q, rem, c, clen);
    return r != nullptr;
}

// note LIKE '%(200%)%'  → contains "(200" and then a ')' anywhere after.
static inline bool note_like_200_paren(const char* s, size_t slen) {
    // Find "(200"
    static const char NEEDLE[] = "(200";
    const char* p = mem_find(s, slen, NEEDLE, 4);
    if (!p) return false;
    p += 4;
    // Search for ')' anywhere after.
    size_t rem = slen - (p - s);
    return memchr(p, ')', rem) != nullptr;
}

// note LIKE '%(USA)%' OR '%(worldwide)%'
static inline bool note_like_usa_or_worldwide(const char* s, size_t slen) {
    if (mem_find(s, slen, "(USA)", 5)) return true;
    if (mem_find(s, slen, "(worldwide)", 11)) return true;
    return false;
}

// info LIKE 'Japan:%2007%' OR 'USA:%2008%'
static inline bool mi_info_match(const char* s, size_t slen) {
    if (slen >= 6 && memcmp(s, "Japan:", 6) == 0) {
        if (mem_find(s + 6, slen - 6, "2007", 4)) return true;
    }
    if (slen >= 4 && memcmp(s, "USA:", 4) == 0) {
        if (mem_find(s + 4, slen - 4, "2008", 4)) return true;
    }
    return false;
}

static inline bool name_has_angel(const char* s, size_t slen) {
    return mem_find(s, slen, "Angel", 5) != nullptr;
}

// Resolve a value in a varlen column by linear scan. Returns 0-based row index
// (which equals id-1 for dense-PK tables).
static int find_varlen_eq(const MmapColumn<uint64_t>& off,
                          const MmapColumn<char>& dat,
                          const char* needle, size_t nlen) {
    size_t n = off.count - 1;
    for (size_t i = 0; i < n; ++i) {
        size_t s = off.data[i], e = off.data[i + 1];
        if (e - s == nlen && memcmp(dat.data + s, needle, nlen) == 0) return (int)i;
    }
    return -1;
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];

    // Ensure results dir exists.
    mkdir(rdir.c_str(), 0755);

    // -----------------------------------------------------------------
    // Phase: mmap data files
    // -----------------------------------------------------------------
    MmapColumn<int32_t> t_year, t_id;
    MmapColumn<uint64_t> t_title_off;
    MmapColumn<char>     t_title_dat;

    MmapColumn<uint64_t> rt_off; MmapColumn<char> rt_dat;
    MmapColumn<uint64_t> it_off; MmapColumn<char> it_dat;

    MmapColumn<uint64_t> cn_cc_dict_off; MmapColumn<char> cn_cc_dict_dat;
    MmapColumn<int16_t>  cn_cc_codes;

    MmapColumn<uint64_t> n_gender_dict_off; MmapColumn<char> n_gender_dict_dat;
    MmapColumn<int8_t>   n_gender;
    MmapColumn<uint64_t> n_name_off; MmapColumn<char> n_name_dat;

    MmapColumn<uint64_t> mc_note_off; MmapColumn<char> mc_note_dat;
    MmapColumn<int32_t>  mc_company_id;

    MmapColumn<int32_t>  mi_info_type_id;
    MmapColumn<uint64_t> mi_info_off; MmapColumn<char> mi_info_dat;

    MmapColumn<int32_t>  ci_role_id, ci_person_id, ci_person_role_id;
    MmapColumn<uint64_t> ci_note_off; MmapColumn<char> ci_note_dat;

    MmapColumn<int32_t> idx_mc, idx_mi, idx_ci, idx_an;

    {
        GENDB_PHASE("data_loading");
        t_year.open(gdir + "/title/production_year.bin");
        t_id.open(gdir + "/title/id.bin");
        t_title_off.open(gdir + "/title/title.off");
        t_title_dat.open(gdir + "/title/title.dat");

        rt_off.open(gdir + "/role_type/role.off");
        rt_dat.open(gdir + "/role_type/role.dat");
        it_off.open(gdir + "/info_type/info.off");
        it_dat.open(gdir + "/info_type/info.dat");

        cn_cc_dict_off.open(gdir + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(gdir + "/company_name/country_code.dict.dat");
        cn_cc_codes.open(gdir + "/company_name/country_code.bin");

        n_gender_dict_off.open(gdir + "/name/gender.dict.off");
        n_gender_dict_dat.open(gdir + "/name/gender.dict.dat");
        n_gender.open(gdir + "/name/gender.bin");
        n_name_off.open(gdir + "/name/name.off");
        n_name_dat.open(gdir + "/name/name.dat");

        mc_note_off.open(gdir + "/movie_companies/note.off");
        mc_note_dat.open(gdir + "/movie_companies/note.dat");
        mc_company_id.open(gdir + "/movie_companies/company_id.bin");

        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info_off.open(gdir + "/movie_info/info.off");
        mi_info_dat.open(gdir + "/movie_info/info.dat");

        ci_role_id.open(gdir + "/cast_info/role_id.bin");
        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_person_role_id.open(gdir + "/cast_info/person_role_id.bin");
        ci_note_off.open(gdir + "/cast_info/note.off");
        ci_note_dat.open(gdir + "/cast_info/note.dat");

        idx_mc.open(gdir + "/_idx/movie_companies__movie_id__offsets.bin");
        idx_mi.open(gdir + "/_idx/movie_info__movie_id__offsets.bin");
        idx_ci.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
        idx_an.open(gdir + "/_idx/aka_name__person_id__offsets.bin");

        // Random-access advisories for big tables we'll probe by range.
        n_name_off.advise_random();
        n_name_dat.advise_random();
        n_gender.advise_random();
        mc_note_off.advise_random();
        mc_note_dat.advise_random();
        mc_company_id.advise_random();
        mi_info_type_id.advise_random();
        mi_info_off.advise_random();
        mi_info_dat.advise_random();
        ci_role_id.advise_random();
        ci_person_id.advise_random();
        ci_person_role_id.advise_random();
        ci_note_off.advise_random();
        ci_note_dat.advise_random();
    }

    // -----------------------------------------------------------------
    // Phase: resolve dim ids
    // -----------------------------------------------------------------
    int rt_actress, it_rd;
    int16_t cc_us_code = 0;
    int8_t  gf_code = 0;
    {
        GENDB_PHASE("resolve_dim_ids");
        int ridx = find_varlen_eq(rt_off, rt_dat, "actress", 7);
        if (ridx < 0) { std::fprintf(stderr, "rt 'actress' not found\n"); return 1; }
        rt_actress = ridx + 1; // dense PK

        int iidx = find_varlen_eq(it_off, it_dat, "release dates", 13);
        if (iidx < 0) { std::fprintf(stderr, "it 'release dates' not found\n"); return 1; }
        it_rd = iidx + 1;

        // company_name country_code dict: find "[us]"
        size_t dn = cn_cc_dict_off.count - 1;
        for (size_t i = 0; i < dn; ++i) {
            size_t s = cn_cc_dict_off.data[i], e = cn_cc_dict_off.data[i + 1];
            if (e - s == 4 && memcmp(cn_cc_dict_dat.data + s, "[us]", 4) == 0) {
                cc_us_code = (int16_t)(i + 1); break;
            }
        }
        if (cc_us_code == 0) { std::fprintf(stderr, "country_code '[us]' not found\n"); return 1; }

        // name gender dict: find "f"
        size_t gn = n_gender_dict_off.count - 1;
        for (size_t i = 0; i < gn; ++i) {
            size_t s = n_gender_dict_off.data[i], e = n_gender_dict_off.data[i + 1];
            if (e - s == 1 && n_gender_dict_dat.data[s] == 'f') {
                gf_code = (int8_t)(i + 1); break;
            }
        }
        if (gf_code == 0) { std::fprintf(stderr, "gender 'f' not found\n"); return 1; }
    }

    // -----------------------------------------------------------------
    // Phase: title scan — collect titles matching year+LIKE
    // -----------------------------------------------------------------
    struct Survivor { int32_t id; const char* title; size_t tlen; };
    std::vector<Survivor> survivors;
    {
        GENDB_PHASE("main_scan");
        size_t nt = t_year.count;
        for (size_t i = 0; i < nt; ++i) {
            int32_t y = t_year.data[i];
            if (y < 2007 || y > 2008) continue; // also skips INT32_MIN (NULL)
            size_t s = t_title_off.data[i];
            size_t e = t_title_off.data[i + 1];
            size_t tlen = e - s;
            const char* ts = t_title_dat.data + s;
            if (!like_three_in_order(ts, tlen,
                                     "Kung", 4, "Fu", 2, "Panda", 5)) continue;
            survivors.push_back({(int32_t)(i + 1), ts, tlen});
        }
    }

    // -----------------------------------------------------------------
    // Phase: per-survivor join and aggregate
    // -----------------------------------------------------------------
    std::string min_name; bool have_min_name = false;
    std::string min_title; bool have_min_title = false;

    {
        GENDB_PHASE("probe_and_aggregate");

        const int32_t* idx_mc_p = idx_mc.data;
        const int32_t* idx_mi_p = idx_mi.data;
        const int32_t* idx_ci_p = idx_ci.data;
        const int32_t* idx_an_p = idx_an.data;

        for (const auto& sv : survivors) {
            int32_t tid = sv.id;

            // --- mc semi-join ---
            int32_t mc_lo = idx_mc_p[tid];
            int32_t mc_hi = idx_mc_p[tid + 1];
            bool mc_ok = false;
            for (int32_t r = mc_lo; r < mc_hi && !mc_ok; ++r) {
                size_t ns = mc_note_off.data[r];
                size_t ne = mc_note_off.data[r + 1];
                size_t nlen = ne - ns;
                if (nlen == 0) continue; // note null
                const char* ns_p = mc_note_dat.data + ns;
                if (!note_like_200_paren(ns_p, nlen)) continue;
                if (!note_like_usa_or_worldwide(ns_p, nlen)) continue;
                int32_t cid = mc_company_id.data[r];
                if (cid <= 0) continue;
                if (cn_cc_codes.data[cid - 1] != cc_us_code) continue;
                mc_ok = true;
            }
            if (!mc_ok) continue;

            // --- mi semi-join ---
            int32_t mi_lo = idx_mi_p[tid];
            int32_t mi_hi = idx_mi_p[tid + 1];
            bool mi_ok = false;
            for (int32_t r = mi_lo; r < mi_hi && !mi_ok; ++r) {
                if (mi_info_type_id.data[r] != it_rd) continue;
                size_t is = mi_info_off.data[r];
                size_t ie = mi_info_off.data[r + 1];
                size_t ilen = ie - is;
                if (ilen == 0) continue; // info IS NULL
                const char* ip = mi_info_dat.data + is;
                if (!mi_info_match(ip, ilen)) continue;
                mi_ok = true;
            }
            if (!mi_ok) continue;

            // --- ci scan; produce candidate rows ---
            int32_t ci_lo = idx_ci_p[tid];
            int32_t ci_hi = idx_ci_p[tid + 1];
            for (int32_t r = ci_lo; r < ci_hi; ++r) {
                if (ci_role_id.data[r] != rt_actress) continue;
                if (ci_person_role_id.data[r] == INT32_MIN) continue;
                size_t ns = ci_note_off.data[r];
                size_t ne = ci_note_off.data[r + 1];
                if (ne - ns != 7) continue;
                if (memcmp(ci_note_dat.data + ns, "(voice)", 7) != 0) continue;

                int32_t pid = ci_person_id.data[r];
                if (pid <= 0) continue;
                // name lookup (dense PK)
                if (n_gender.data[pid - 1] != gf_code) continue;
                size_t nos = n_name_off.data[pid - 1];
                size_t noe = n_name_off.data[pid];
                size_t nlen = noe - nos;
                const char* np = n_name_dat.data + nos;
                if (!name_has_angel(np, nlen)) continue;
                // aka_name existence (offsets_only) — pid is dense parent id
                if (idx_an_p[pid + 1] <= idx_an_p[pid]) continue;

                // Candidate: update mins.
                if (!have_min_name ||
                    std::lexicographical_compare(np, np + nlen,
                                                 min_name.data(),
                                                 min_name.data() + min_name.size())) {
                    min_name.assign(np, nlen);
                    have_min_name = true;
                }
                if (!have_min_title ||
                    std::lexicographical_compare(sv.title, sv.title + sv.tlen,
                                                 min_title.data(),
                                                 min_title.data() + min_title.size())) {
                    min_title.assign(sv.title, sv.tlen);
                    have_min_title = true;
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Phase: output CSV
    // -----------------------------------------------------------------
    {
        GENDB_PHASE("output");
        std::string path = rdir + "/Q19b.csv";
        FILE* fp = std::fopen(path.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
        std::fprintf(fp, "voicing_actress,kung_fu_panda\n");
        if (have_min_name && have_min_title) {
            auto emit = [&](const std::string& v) {
                bool need_quote = v.find(',') != std::string::npos ||
                                  v.find('"') != std::string::npos ||
                                  v.find('\n') != std::string::npos;
                if (!need_quote) { std::fwrite(v.data(), 1, v.size(), fp); return; }
                std::fputc('"', fp);
                for (char c : v) {
                    if (c == '"') std::fputc('"', fp);
                    std::fputc(c, fp);
                }
                std::fputc('"', fp);
            };
            emit(min_name);
            std::fputc(',', fp);
            emit(min_title);
            std::fputc('\n', fp);
        } else {
            std::fprintf(fp, ",\n");
        }
        std::fclose(fp);
    }

    return 0;
}
