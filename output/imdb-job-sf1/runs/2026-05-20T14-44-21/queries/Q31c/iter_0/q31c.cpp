// Q31c — Lionsgate horror/action/etc. with writer credits and MINs.
// Strategy: cn(Lionsgate%) → CSR movie_companies.company_id → mv_set
// Then per mv: mk semi-join (7 keywords) → mi (genres + 6-set) → mi_idx (votes)
// → ci (5 writer-notes) lookup name. Capture MIN(mi.info), MIN(mi_idx.info),
// MIN(n.name), MIN(t.title).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "timing_utils.h"
#include "mmap_utils.h"

#ifdef _OPENMP
#include <omp.h>
#endif

using namespace gendb;

static inline bool memeq(const char* a, const char* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

// Compare varlen [a, a+na) vs [b, b+nb) lexicographically; return <0/0/>0.
static inline int varlen_cmp(const char* a, size_t na, const char* b, size_t nb) {
    size_t n = na < nb ? na : nb;
    int c = std::memcmp(a, b, n);
    if (c != 0) return c;
    if (na == nb) return 0;
    return na < nb ? -1 : 1;
}

struct VarColumn {
    MmapColumn<int64_t> off;
    MmapColumn<char>    dat;
    void open(const std::string& base) {
        off.open(base + ".off");
        dat.open(base + ".dat");
    }
    inline const char* ptr(size_t i) const { return dat.data + off.data[i]; }
    inline size_t len(size_t i) const { return (size_t)(off.data[i+1] - off.data[i]); }
};

// Holder for a running MIN over varlen values (zero-copy: just record best ptr+len)
struct MinVarlen {
    const char* p = nullptr;
    size_t n = 0;
    inline void update(const char* x, size_t xn) {
        if (p == nullptr || varlen_cmp(x, xn, p, n) < 0) {
            p = x; n = xn;
        }
    }
};

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gdir = argv[1];
    std::string rdir = argv[2];
    std::filesystem::create_directories(rdir);

    // ---- Data loading ----
    // Dimension columns
    MmapColumn<int32_t> it_id, k_id, cn_id;
    VarColumn it_info, k_kw, cn_name, name_name;

    // Fact columns
    MmapColumn<int32_t> mc_company_id, mc_movie_id;
    MmapColumn<int32_t> mi_movie_id, mi_info_type_id;
    VarColumn           mi_info;
    MmapColumn<int32_t> mii_movie_id, mii_info_type_id;
    VarColumn           mii_info;
    MmapColumn<int32_t> mk_movie_id, mk_keyword_id;
    MmapColumn<int32_t> ci_movie_id, ci_person_id;
    VarColumn           ci_note;
    VarColumn           t_title;

    // Indexes
    MmapColumn<int32_t> mcc_off, mcc_row;
    MmapColumn<int32_t> mk_off, mi_off, mii_off, ci_off;

    {
        GENDB_PHASE("data_loading");
        it_id.open(gdir + "/info_type/id.bin");
        it_info.open(gdir + "/info_type/info");

        k_id.open(gdir + "/keyword/id.bin");
        k_kw.open(gdir + "/keyword/keyword");

        cn_id.open(gdir + "/company_name/id.bin");
        cn_name.open(gdir + "/company_name/name");

        name_name.open(gdir + "/name/name");

        mc_company_id.open(gdir + "/movie_companies/company_id.bin");
        mc_movie_id.open(gdir + "/movie_companies/movie_id.bin");

        mi_movie_id.open(gdir + "/movie_info/movie_id.bin");
        mi_info_type_id.open(gdir + "/movie_info/info_type_id.bin");
        mi_info.open(gdir + "/movie_info/info");

        mii_movie_id.open(gdir + "/movie_info_idx/movie_id.bin");
        mii_info_type_id.open(gdir + "/movie_info_idx/info_type_id.bin");
        mii_info.open(gdir + "/movie_info_idx/info");

        mk_movie_id.open(gdir + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gdir + "/movie_keyword/keyword_id.bin");

        ci_movie_id.open(gdir + "/cast_info/movie_id.bin");
        ci_person_id.open(gdir + "/cast_info/person_id.bin");
        ci_note.open(gdir + "/cast_info/note");

        t_title.open(gdir + "/title/title");

        mcc_off.open(gdir + "/_idx/movie_companies__company_id__offsets.bin");
        mcc_row.open(gdir + "/_idx/movie_companies__company_id__rowids.bin");
        mk_off.open(gdir + "/_idx/movie_keyword__movie_id__offsets.bin");
        mi_off.open(gdir + "/_idx/movie_info__movie_id__offsets.bin");
        mii_off.open(gdir + "/_idx/movie_info_idx__movie_id__offsets.bin");
        ci_off.open(gdir + "/_idx/cast_info__movie_id__offsets.bin");
    }

    // ---- Resolve info_type ids ----
    int32_t it1_id = -1; // genres
    int32_t it2_id = -1; // votes
    {
        GENDB_PHASE("resolve_info_types");
        for (size_t i = 0; i < it_id.count; ++i) {
            const char* p = it_info.ptr(i);
            size_t n = it_info.len(i);
            if (n == 6 && memeq(p, "genres", 6)) it1_id = it_id.data[i];
            else if (n == 5 && memeq(p, "votes", 5)) it2_id = it_id.data[i];
        }
        if (it1_id < 0 || it2_id < 0) {
            std::fprintf(stderr, "info_type ids not resolved: it1=%d it2=%d\n", it1_id, it2_id);
            return 2;
        }
    }

    // ---- Resolve keyword ids (7-set) ----
    static const char* KW_LITS[] = {
        "murder", "violence", "blood", "gore", "death", "female-nudity", "hospital"
    };
    static const size_t KW_LENS[] = {6, 8, 5, 4, 5, 13, 8};
    const size_t NK = 7;

    int32_t k_ids[7];
    int n_k_ids = 0;
    {
        GENDB_PHASE("resolve_keywords");
        for (size_t i = 0; i < k_id.count; ++i) {
            const char* p = k_kw.ptr(i);
            size_t nlen = k_kw.len(i);
            for (size_t j = 0; j < NK; ++j) {
                if (nlen == KW_LENS[j] && memeq(p, KW_LITS[j], KW_LENS[j])) {
                    k_ids[n_k_ids++] = k_id.data[i];
                    break;
                }
            }
        }
    }

    // ---- Scan company_name for 'Lionsgate%' ----
    static const char* PREFIX = "Lionsgate";
    static const size_t PLEN = 9;
    std::vector<int32_t> cn_ids;
    cn_ids.reserve(64);
    {
        GENDB_PHASE("scan_company_name_prefix");
        size_t N = cn_id.count;
        for (size_t i = 0; i < N; ++i) {
            size_t nlen = cn_name.len(i);
            if (nlen >= PLEN) {
                const char* p = cn_name.ptr(i);
                if (memeq(p, PREFIX, PLEN)) {
                    cn_ids.push_back(cn_id.data[i]);
                }
            }
        }
    }

    // ---- Expand CSR mc.company_id -> mv_set ----
    std::unordered_set<int32_t> mv_set;
    mv_set.reserve(8192);
    {
        GENDB_PHASE("expand_movie_companies_by_company");
        size_t mcc_off_count = mcc_off.count;
        for (int32_t cid : cn_ids) {
            if (cid < 0 || (size_t)cid + 1 >= mcc_off_count) continue;
            int32_t lo = mcc_off.data[cid];
            int32_t hi = mcc_off.data[cid + 1];
            for (int32_t k = lo; k < hi; ++k) {
                int32_t r = mcc_row.data[k];
                int32_t mv = mc_movie_id.data[r];
                mv_set.insert(mv);
            }
        }
    }

    // ---- Filter sets ----
    // mi.info IN {Horror,Action,Sci-Fi,Thriller,Crime,War}
    static const char* GENRES[] = {"Horror","Action","Sci-Fi","Thriller","Crime","War"};
    static const size_t GLEN[]  = {6,       6,       6,       8,         5,      3};
    const size_t NG = 6;

    // ci.note IN 5-writer-set
    static const char* NOTES[] = {"(writer)","(head writer)","(written by)","(story)","(story editor)"};
    static const size_t NLEN[] = {8,          13,             12,            7,        15};
    const size_t NN = 5;

    // ---- Main per-mv loop with thread-local MINs ----
    // We track per-mv survivors of mk(semi), mi(filter), mii(filter), and ci(filter).
    // A movie qualifies if all four conditions exist (≥1 matching row in each).
    // Per qualifying mv, MIN aggregates from each table's matching rows.

    // For parallelism: copy mv_set into a vector
    std::vector<int32_t> mvs(mv_set.begin(), mv_set.end());
    size_t M = mvs.size();

    size_t off_n_mk = mk_off.count;
    size_t off_n_mi = mi_off.count;
    size_t off_n_mii = mii_off.count;
    size_t off_n_ci = ci_off.count;
    size_t title_off_n = t_title.off.count; // = N+1

    int n_threads = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif

    struct LocalMins {
        MinVarlen mi_info_min;
        MinVarlen mii_info_min;
        MinVarlen name_min;
        MinVarlen title_min;
    };
    std::vector<LocalMins> tl(n_threads);

    {
        GENDB_PHASE("main_scan");
        #pragma omp parallel
        {
            int tid = 0;
#ifdef _OPENMP
            tid = omp_get_thread_num();
#endif
            LocalMins& L = tl[tid];

            #pragma omp for schedule(dynamic, 64)
            for (size_t mi_idx = 0; mi_idx < M; ++mi_idx) {
                int32_t mv = mvs[mi_idx];
                if (mv <= 0) continue;

                // -- mk semi-join: any mk row for mv with keyword_id ∈ k_ids
                if ((size_t)mv + 1 >= off_n_mk) continue;
                int32_t mk_lo = mk_off.data[mv];
                int32_t mk_hi = mk_off.data[mv + 1];
                bool mk_ok = false;
                for (int32_t r = mk_lo; r < mk_hi; ++r) {
                    int32_t kid = mk_keyword_id.data[r];
                    for (int j = 0; j < n_k_ids; ++j) {
                        if (kid == k_ids[j]) { mk_ok = true; break; }
                    }
                    if (mk_ok) break;
                }
                if (!mk_ok) continue;

                // -- mi filter capture: info_type_id==it1_id AND info ∈ 6-set
                if ((size_t)mv + 1 >= off_n_mi) continue;
                int32_t mi_lo = mi_off.data[mv];
                int32_t mi_hi = mi_off.data[mv + 1];
                MinVarlen mi_local;
                for (int32_t r = mi_lo; r < mi_hi; ++r) {
                    if (mi_info_type_id.data[r] != it1_id) continue;
                    size_t ilen = mi_info.len(r);
                    const char* ip = mi_info.ptr(r);
                    for (size_t j = 0; j < NG; ++j) {
                        if (ilen == GLEN[j] && memeq(ip, GENRES[j], GLEN[j])) {
                            mi_local.update(ip, ilen);
                            break;
                        }
                    }
                }
                if (mi_local.p == nullptr) continue;

                // -- mi_idx filter capture: info_type_id==it2_id
                if ((size_t)mv + 1 >= off_n_mii) continue;
                int32_t mii_lo = mii_off.data[mv];
                int32_t mii_hi = mii_off.data[mv + 1];
                MinVarlen mii_local;
                for (int32_t r = mii_lo; r < mii_hi; ++r) {
                    if (mii_info_type_id.data[r] != it2_id) continue;
                    size_t ilen = mii_info.len(r);
                    const char* ip = mii_info.ptr(r);
                    mii_local.update(ip, ilen);
                }
                if (mii_local.p == nullptr) continue;

                // -- ci filter capture: note ∈ 5-set, name via person_id-1
                if ((size_t)mv + 1 >= off_n_ci) continue;
                int32_t ci_lo = ci_off.data[mv];
                int32_t ci_hi = ci_off.data[mv + 1];
                MinVarlen name_local;
                for (int32_t r = ci_lo; r < ci_hi; ++r) {
                    // note check
                    size_t nlen = ci_note.len(r);
                    bool nok = false;
                    if (nlen > 0) {
                        const char* np = ci_note.ptr(r);
                        for (size_t j = 0; j < NN; ++j) {
                            if (nlen == NLEN[j] && memeq(np, NOTES[j], NLEN[j])) {
                                nok = true; break;
                            }
                        }
                    }
                    if (!nok) continue;
                    int32_t pid = ci_person_id.data[r];
                    if (pid <= 0) continue;
                    size_t pi = (size_t)(pid - 1);
                    if (pi + 1 >= name_name.off.count) continue;
                    size_t nm_len = name_name.len(pi);
                    const char* nm_p = name_name.ptr(pi);
                    name_local.update(nm_p, nm_len);
                }
                if (name_local.p == nullptr) continue;

                // -- title for mv (mv is 1-based)
                size_t ti = (size_t)(mv - 1);
                if (ti + 1 >= title_off_n) continue;
                size_t tlen = t_title.len(ti);
                const char* tp = t_title.ptr(ti);

                // Merge into thread-local mins
                L.mi_info_min.update(mi_local.p, mi_local.n);
                L.mii_info_min.update(mii_local.p, mii_local.n);
                L.name_min.update(name_local.p, name_local.n);
                L.title_min.update(tp, tlen);
            }
        }
    }

    // Reduce thread-local mins
    MinVarlen MI, MII, NM, TT;
    for (int t = 0; t < n_threads; ++t) {
        if (tl[t].mi_info_min.p)  MI.update(tl[t].mi_info_min.p,  tl[t].mi_info_min.n);
        if (tl[t].mii_info_min.p) MII.update(tl[t].mii_info_min.p, tl[t].mii_info_min.n);
        if (tl[t].name_min.p)     NM.update(tl[t].name_min.p,      tl[t].name_min.n);
        if (tl[t].title_min.p)    TT.update(tl[t].title_min.p,     tl[t].title_min.n);
    }

    // ---- Output ----
    {
        GENDB_PHASE("output");
        std::string out_path = rdir + "/Q31c.csv";
        std::ofstream f(out_path);
        f << "movie_budget,movie_votes,writer,violent_liongate_movie\n";

        auto emit_field = [&](const char* p, size_t n) {
            if (!p) { return; }
            // CSV quoting: if value contains comma, quote, or newline, wrap in quotes and double-quote any quotes.
            bool needq = false;
            for (size_t i = 0; i < n; ++i) {
                char c = p[i];
                if (c == ',' || c == '"' || c == '\n' || c == '\r') { needq = true; break; }
            }
            if (!needq) {
                f.write(p, n);
            } else {
                f.put('"');
                for (size_t i = 0; i < n; ++i) {
                    char c = p[i];
                    if (c == '"') f.put('"');
                    f.put(c);
                }
                f.put('"');
            }
        };

        emit_field(MI.p, MI.n);
        f.put(',');
        emit_field(MII.p, MII.n);
        f.put(',');
        emit_field(NM.p, NM.n);
        f.put(',');
        emit_field(TT.p, TT.n);
        f.put('\n');
        f.close();
    }

    return 0;
}
