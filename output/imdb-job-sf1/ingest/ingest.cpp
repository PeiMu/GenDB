// IMDB-JOB SF1 ingestion: parses CSVs into binary columnar layout.
// One worker thread per table; large tables run alongside smaller ones.

#include "common.h"
#include <functional>
#include <queue>

// =============================================================================
// Per-column buffers built during a single sequential pass over each table.
// =============================================================================

// --------- Helpers ----------------------------------------------------------

// Sort `key` ascending; produce permutation `perm` such that key[perm[i]] is sorted.
static std::vector<int32_t> argsort_int(const std::vector<int32_t>& key) {
    std::vector<int32_t> p(key.size());
    std::iota(p.begin(), p.end(), 0);
    std::sort(p.begin(), p.end(),
              [&](int32_t a, int32_t b){ return key[a] < key[b]; });
    return p;
}

static std::vector<int32_t> reorder_int(const std::vector<int32_t>& v,
                                        const std::vector<int32_t>& perm) {
    std::vector<int32_t> out(perm.size());
    for (size_t i = 0; i < perm.size(); ++i) out[i] = v[perm[i]];
    return out;
}

// Place rows by dense PK id (1..N). Returns reordered int column.
static std::vector<int32_t> place_by_id(const std::vector<int32_t>& ids,
                                        const std::vector<int32_t>& col,
                                        int32_t max_id) {
    std::vector<int32_t> out(max_id, NULL_INT);
    for (size_t i = 0; i < ids.size(); ++i) {
        int32_t id = ids[i];
        if (id < 1 || id > max_id) { fprintf(stderr,"id %d out of range %d\n", id, max_id); exit(1); }
        out[id - 1] = col[i];
    }
    return out;
}

static VarlenCol place_varlen_by_id(const std::vector<int32_t>& ids,
                                    const VarlenCol& col, int32_t max_id) {
    VarlenCol out;
    out.offsets.assign(max_id + 1, 0);
    // Compute lengths in id order.
    std::vector<int32_t> len(max_id, 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        int32_t id = ids[i];
        int64_t s = col.offsets[i], e = col.offsets[i+1];
        len[id - 1] = (int32_t)(e - s);
    }
    int64_t total = 0;
    for (int32_t i = 0; i < max_id; ++i) { out.offsets[i] = total; total += len[i]; }
    out.offsets[max_id] = total;
    out.data.resize(total);
    for (size_t i = 0; i < ids.size(); ++i) {
        int32_t id = ids[i];
        int64_t src_s = col.offsets[i], src_e = col.offsets[i+1];
        int64_t dst_s = out.offsets[id - 1];
        memcpy(out.data.data() + dst_s, col.data.data() + src_s, src_e - src_s);
    }
    return out;
}

static void write_int32(const std::string& dir, const std::string& col,
                        const std::vector<int32_t>& v) {
    write_vec(dir + "/" + col + ".bin", v);
}

static void write_int8(const std::string& dir, const std::string& col,
                       const std::vector<int8_t>& v) {
    write_vec(dir + "/" + col + ".bin", v);
}

static void write_int16(const std::string& dir, const std::string& col,
                        const std::vector<int16_t>& v) {
    write_vec(dir + "/" + col + ".bin", v);
}

// Dictionary builder: assigns codes (1..K). Empty string -> code 0 (NULL).
struct DictBuilder {
    std::unordered_map<std::string, int32_t> map;
    VarlenCol dict { 0, 0 };
    int32_t encode(std::string_view sv) {
        if (sv.empty()) return 0;
        std::string s(sv);
        auto it = map.find(s);
        if (it != map.end()) return it->second;
        int32_t code = (int32_t)map.size() + 1;
        map.emplace(s, code);
        dict.append(sv);
        return code;
    }
    void write(const std::string& dir, const std::string& col) const {
        write_vec(dir + "/" + col + ".dict.off", dict.offsets);
        write_file(dir + "/" + col + ".dict.dat", dict.data.data(), dict.data.size());
    }
};

// =============================================================================
// Per-table ingest functions.
// =============================================================================

struct Ctx {
    std::string in_dir;   // path to /sf1
    std::string out_dir;  // path to storage root
};

// ---- title ------------------------------------------------------------------
static void ingest_title(const Ctx& c) {
    constexpr int32_t MAXID = 2528312;
    std::string tdir = c.out_dir + "/title"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/title.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids; ids.reserve(MAXID);
    VarlenCol title_c(MAXID, 60ull * MAXID);
    VarlenCol imdb_index_c(MAXID, 4ull * MAXID);
    std::vector<int32_t> kind_id; kind_id.reserve(MAXID);
    std::vector<int32_t> production_year; production_year.reserve(MAXID);
    std::vector<int32_t> imdb_id; imdb_id.reserve(MAXID);
    VarlenCol phonetic_code_c(MAXID, 4ull * MAXID);
    std::vector<int32_t> episode_of_id; episode_of_id.reserve(MAXID);
    std::vector<int32_t> season_nr; season_nr.reserve(MAXID);
    std::vector<int32_t> episode_nr; episode_nr.reserve(MAXID);
    VarlenCol series_years_c(MAXID, 4ull * MAXID);
    VarlenCol md5sum_c(MAXID, 33ull * MAXID);

    while (p.next()) {
        if (p.fields.size() < 12) { fprintf(stderr, "title bad row, %zu fields\n", p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        title_c.append(p.fields[1]);
        imdb_index_c.append(p.fields[2]);
        kind_id.push_back(parse_int_or_null(p.fields[3]));
        production_year.push_back(parse_int_or_null(p.fields[4]));
        imdb_id.push_back(parse_int_or_null(p.fields[5]));
        phonetic_code_c.append(p.fields[6]);
        episode_of_id.push_back(parse_int_or_null(p.fields[7]));
        season_nr.push_back(parse_int_or_null(p.fields[8]));
        episode_nr.push_back(parse_int_or_null(p.fields[9]));
        series_years_c.append(p.fields[10]);
        md5sum_c.append(p.fields[11]);
    }
    logmsg("title: parsed %zu rows", ids.size());
    // Place by id.
    std::vector<int32_t> id_out(MAXID);
    for (int32_t i = 0; i < MAXID; ++i) id_out[i] = i + 1;
    write_int32(tdir, "id", id_out);
    write_int32(tdir, "kind_id",         place_by_id(ids, kind_id, MAXID));
    write_int32(tdir, "production_year", place_by_id(ids, production_year, MAXID));
    write_int32(tdir, "imdb_id",         place_by_id(ids, imdb_id, MAXID));
    write_int32(tdir, "episode_of_id",   place_by_id(ids, episode_of_id, MAXID));
    write_int32(tdir, "season_nr",       place_by_id(ids, season_nr, MAXID));
    write_int32(tdir, "episode_nr",      place_by_id(ids, episode_nr, MAXID));
    place_varlen_by_id(ids, title_c, MAXID).write(tdir, "title");
    place_varlen_by_id(ids, imdb_index_c, MAXID).write(tdir, "imdb_index");
    place_varlen_by_id(ids, phonetic_code_c, MAXID).write(tdir, "phonetic_code");
    place_varlen_by_id(ids, series_years_c, MAXID).write(tdir, "series_years");
    place_varlen_by_id(ids, md5sum_c, MAXID).write(tdir, "md5sum");
    logmsg("title: wrote columns");
}

// ---- name ------------------------------------------------------------------
static void ingest_name(const Ctx& c) {
    constexpr int32_t MAXID = 4167491;
    std::string tdir = c.out_dir + "/name"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/name.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids; ids.reserve(MAXID);
    VarlenCol name_c(MAXID, 30ull * MAXID);
    VarlenCol imdb_index_c(MAXID, 4ull * MAXID);
    std::vector<int32_t> imdb_id; imdb_id.reserve(MAXID);
    DictBuilder gender_d;
    std::vector<int8_t> gender_code; gender_code.reserve(MAXID);
    VarlenCol pcode_cf(MAXID, 6ull * MAXID);
    VarlenCol pcode_nf(MAXID, 6ull * MAXID);
    VarlenCol surname_pcode(MAXID, 6ull * MAXID);
    VarlenCol md5sum_c(MAXID, 33ull * MAXID);

    while (p.next()) {
        if (p.fields.size() < 9) { fprintf(stderr,"name bad row %zu\n", p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        name_c.append(p.fields[1]);
        imdb_index_c.append(p.fields[2]);
        imdb_id.push_back(parse_int_or_null(p.fields[3]));
        gender_code.push_back((int8_t)gender_d.encode(p.fields[4]));
        pcode_cf.append(p.fields[5]);
        pcode_nf.append(p.fields[6]);
        surname_pcode.append(p.fields[7]);
        md5sum_c.append(p.fields[8]);
    }
    logmsg("name: parsed %zu rows, gender dict size=%zu", ids.size(), gender_d.map.size());

    // Place by id.
    std::vector<int32_t> id_out(MAXID); for (int32_t i=0;i<MAXID;++i) id_out[i]=i+1;
    write_int32(tdir, "id", id_out);
    write_int32(tdir, "imdb_id", place_by_id(ids, imdb_id, MAXID));

    // gender: int8 placed by id (NULL = 0; dict starts at 1).
    std::vector<int8_t> gender_out(MAXID, 0);
    for (size_t i = 0; i < ids.size(); ++i) gender_out[ids[i]-1] = gender_code[i];
    write_int8(tdir, "gender", gender_out);
    gender_d.write(tdir, "gender");

    place_varlen_by_id(ids, name_c, MAXID).write(tdir, "name");
    place_varlen_by_id(ids, imdb_index_c, MAXID).write(tdir, "imdb_index");
    place_varlen_by_id(ids, pcode_cf, MAXID).write(tdir, "name_pcode_cf");
    place_varlen_by_id(ids, pcode_nf, MAXID).write(tdir, "name_pcode_nf");
    place_varlen_by_id(ids, surname_pcode, MAXID).write(tdir, "surname_pcode");
    place_varlen_by_id(ids, md5sum_c, MAXID).write(tdir, "md5sum");
    logmsg("name: wrote columns");
}

// ---- char_name ------------------------------------------------------------------
static void ingest_char_name(const Ctx& c) {
    constexpr int32_t MAXID = 3140339;
    std::string tdir = c.out_dir + "/char_name"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/char_name.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids; ids.reserve(MAXID);
    VarlenCol name_c(MAXID, 25ull * MAXID);
    VarlenCol imdb_index_c(MAXID, 4ull * MAXID);
    std::vector<int32_t> imdb_id; imdb_id.reserve(MAXID);
    VarlenCol pcode_nf(MAXID, 6ull * MAXID);
    VarlenCol surname_pcode(MAXID, 6ull * MAXID);
    VarlenCol md5sum_c(MAXID, 33ull * MAXID);

    while (p.next()) {
        if (p.fields.size() < 7) { fprintf(stderr,"char_name bad row %zu\n", p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        name_c.append(p.fields[1]);
        imdb_index_c.append(p.fields[2]);
        imdb_id.push_back(parse_int_or_null(p.fields[3]));
        pcode_nf.append(p.fields[4]);
        surname_pcode.append(p.fields[5]);
        md5sum_c.append(p.fields[6]);
    }
    logmsg("char_name: parsed %zu rows", ids.size());

    std::vector<int32_t> id_out(MAXID); for (int32_t i=0;i<MAXID;++i) id_out[i]=i+1;
    write_int32(tdir, "id", id_out);
    write_int32(tdir, "imdb_id", place_by_id(ids, imdb_id, MAXID));
    place_varlen_by_id(ids, name_c, MAXID).write(tdir, "name");
    place_varlen_by_id(ids, imdb_index_c, MAXID).write(tdir, "imdb_index");
    place_varlen_by_id(ids, pcode_nf, MAXID).write(tdir, "name_pcode_nf");
    place_varlen_by_id(ids, surname_pcode, MAXID).write(tdir, "surname_pcode");
    place_varlen_by_id(ids, md5sum_c, MAXID).write(tdir, "md5sum");
    logmsg("char_name: wrote columns");
}

// ---- keyword ------------------------------------------------------------------
static void ingest_keyword(const Ctx& c) {
    constexpr int32_t MAXID = 134170;
    std::string tdir = c.out_dir + "/keyword"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/keyword.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids;
    VarlenCol kw_c, ph_c;

    while (p.next()) {
        if (p.fields.size() < 3) { fprintf(stderr,"keyword bad row %zu\n", p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        kw_c.append(p.fields[1]);
        ph_c.append(p.fields[2]);
    }
    logmsg("keyword: parsed %zu rows", ids.size());

    std::vector<int32_t> id_out(MAXID); for (int32_t i=0;i<MAXID;++i) id_out[i]=i+1;
    write_int32(tdir, "id", id_out);
    place_varlen_by_id(ids, kw_c, MAXID).write(tdir, "keyword");
    place_varlen_by_id(ids, ph_c, MAXID).write(tdir, "phonetic_code");
}

// ---- company_name ------------------------------------------------------------------
static void ingest_company_name(const Ctx& c) {
    constexpr int32_t MAXID = 234997;
    std::string tdir = c.out_dir + "/company_name"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/company_name.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids;
    VarlenCol name_c;
    DictBuilder country_d;
    std::vector<int16_t> country_code;
    std::vector<int32_t> imdb_id;
    VarlenCol pcode_nf, pcode_sf, md5sum_c;

    while (p.next()) {
        if (p.fields.size() < 7) { fprintf(stderr,"company_name bad row %zu\n", p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        name_c.append(p.fields[1]);
        country_code.push_back((int16_t)country_d.encode(p.fields[2]));
        imdb_id.push_back(parse_int_or_null(p.fields[3]));
        pcode_nf.append(p.fields[4]);
        pcode_sf.append(p.fields[5]);
        md5sum_c.append(p.fields[6]);
    }
    logmsg("company_name: parsed %zu rows, country dict=%zu", ids.size(), country_d.map.size());

    std::vector<int32_t> id_out(MAXID); for (int32_t i=0;i<MAXID;++i) id_out[i]=i+1;
    write_int32(tdir, "id", id_out);
    write_int32(tdir, "imdb_id", place_by_id(ids, imdb_id, MAXID));

    std::vector<int16_t> cc_out(MAXID, 0);
    for (size_t i = 0; i < ids.size(); ++i) cc_out[ids[i]-1] = country_code[i];
    write_int16(tdir, "country_code", cc_out);
    country_d.write(tdir, "country_code");

    place_varlen_by_id(ids, name_c, MAXID).write(tdir, "name");
    place_varlen_by_id(ids, pcode_nf, MAXID).write(tdir, "name_pcode_nf");
    place_varlen_by_id(ids, pcode_sf, MAXID).write(tdir, "name_pcode_sf");
    place_varlen_by_id(ids, md5sum_c, MAXID).write(tdir, "md5sum");
}

// ---- info_type / kind_type / link_type / role_type / company_type / comp_cast_type ----
// All have schema (id, single_text_col). Dense small dimensions.
static void ingest_small_dim(const Ctx& c, const char* tbl,
                             const char* second_col, int32_t maxid) {
    std::string tdir = c.out_dir + "/" + tbl; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/" + tbl + ".csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> ids;
    VarlenCol v;
    while (p.next()) {
        if (p.fields.size() < 2) { fprintf(stderr,"%s bad row %zu\n", tbl, p.fields.size()); exit(1); }
        ids.push_back(parse_int_or_null(p.fields[0]));
        v.append(p.fields[1]);
    }
    std::vector<int32_t> id_out(maxid); for (int32_t i=0;i<maxid;++i) id_out[i]=i+1;
    write_int32(tdir, "id", id_out);
    place_varlen_by_id(ids, v, maxid).write(tdir, second_col);
    logmsg("%s: %zu rows", tbl, ids.size());
}

// ---- generic fact-table loader (for FK-sorted tables) ------------------------
// Each call parses entire CSV into per-column buffers, then sorts by sort_key.
// We pass in column descriptors via a small struct.

struct IntColBuf {
    std::vector<int32_t> v;
    std::string name;
};

// ---- aka_name (sorted by person_id) ------------------------------------------
static void ingest_aka_name(const Ctx& c) {
    std::string tdir = c.out_dir + "/aka_name"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/aka_name.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, person_id;
    VarlenCol name_c, imdb_index_c, pcode_cf, pcode_nf, surname_pcode, md5sum_c;
    while (p.next()) {
        if (p.fields.size() < 8) { fprintf(stderr,"aka_name bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        person_id.push_back(parse_int_or_null(p.fields[1]));
        name_c.append(p.fields[2]);
        imdb_index_c.append(p.fields[3]);
        pcode_cf.append(p.fields[4]);
        pcode_nf.append(p.fields[5]);
        surname_pcode.append(p.fields[6]);
        md5sum_c.append(p.fields[7]);
    }
    logmsg("aka_name: parsed %zu rows", id.size());
    auto perm = argsort_int(person_id);
    write_int32(tdir, "id",         reorder_int(id, perm));
    write_int32(tdir, "person_id",  reorder_int(person_id, perm));
    name_c.reorder(perm).write(tdir, "name");
    imdb_index_c.reorder(perm).write(tdir, "imdb_index");
    pcode_cf.reorder(perm).write(tdir, "name_pcode_cf");
    pcode_nf.reorder(perm).write(tdir, "name_pcode_nf");
    surname_pcode.reorder(perm).write(tdir, "surname_pcode");
    md5sum_c.reorder(perm).write(tdir, "md5sum");
}

// ---- aka_title (sorted by movie_id) ------------------------------------------
static void ingest_aka_title(const Ctx& c) {
    std::string tdir = c.out_dir + "/aka_title"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/aka_title.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, kind_id, production_year, episode_of_id, season_nr, episode_nr;
    VarlenCol title_c, imdb_index_c, phonetic_code_c, note_c, md5sum_c;
    while (p.next()) {
        if (p.fields.size() < 12) { fprintf(stderr,"aka_title bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        title_c.append(p.fields[2]);
        imdb_index_c.append(p.fields[3]);
        kind_id.push_back(parse_int_or_null(p.fields[4]));
        production_year.push_back(parse_int_or_null(p.fields[5]));
        phonetic_code_c.append(p.fields[6]);
        episode_of_id.push_back(parse_int_or_null(p.fields[7]));
        season_nr.push_back(parse_int_or_null(p.fields[8]));
        episode_nr.push_back(parse_int_or_null(p.fields[9]));
        note_c.append(p.fields[10]);
        md5sum_c.append(p.fields[11]);
    }
    logmsg("aka_title: parsed %zu rows", id.size());
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",              reorder_int(id, perm));
    write_int32(tdir, "movie_id",        reorder_int(movie_id, perm));
    write_int32(tdir, "kind_id",         reorder_int(kind_id, perm));
    write_int32(tdir, "production_year", reorder_int(production_year, perm));
    write_int32(tdir, "episode_of_id",   reorder_int(episode_of_id, perm));
    write_int32(tdir, "season_nr",       reorder_int(season_nr, perm));
    write_int32(tdir, "episode_nr",      reorder_int(episode_nr, perm));
    title_c.reorder(perm).write(tdir, "title");
    imdb_index_c.reorder(perm).write(tdir, "imdb_index");
    phonetic_code_c.reorder(perm).write(tdir, "phonetic_code");
    note_c.reorder(perm).write(tdir, "note");
    md5sum_c.reorder(perm).write(tdir, "md5sum");
}

// ---- cast_info (sorted by movie_id, 36M rows) --------------------------------
static void ingest_cast_info(const Ctx& c) {
    constexpr size_t N = 36244344;
    std::string tdir = c.out_dir + "/cast_info"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/cast_info.csv");
    CsvParser p; p.init(mf.data, mf.size);

    std::vector<int32_t> id; id.reserve(N);
    std::vector<int32_t> person_id; person_id.reserve(N);
    std::vector<int32_t> movie_id; movie_id.reserve(N);
    std::vector<int32_t> person_role_id; person_role_id.reserve(N);
    VarlenCol note_c(N, 8ull * N);
    std::vector<int32_t> nr_order; nr_order.reserve(N);
    std::vector<int32_t> role_id; role_id.reserve(N);
    while (p.next()) {
        if (p.fields.size() < 7) { fprintf(stderr,"cast_info bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        person_id.push_back(parse_int_or_null(p.fields[1]));
        movie_id.push_back(parse_int_or_null(p.fields[2]));
        person_role_id.push_back(parse_int_or_null(p.fields[3]));
        note_c.append(p.fields[4]);
        nr_order.push_back(parse_int_or_null(p.fields[5]));
        role_id.push_back(parse_int_or_null(p.fields[6]));
    }
    logmsg("cast_info: parsed %zu rows", id.size());
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",             reorder_int(id, perm));
    write_int32(tdir, "person_id",      reorder_int(person_id, perm));
    write_int32(tdir, "movie_id",       reorder_int(movie_id, perm));
    write_int32(tdir, "person_role_id", reorder_int(person_role_id, perm));
    write_int32(tdir, "nr_order",       reorder_int(nr_order, perm));
    write_int32(tdir, "role_id",        reorder_int(role_id, perm));
    note_c.reorder(perm).write(tdir, "note");
    logmsg("cast_info: wrote columns");
}

// ---- complete_cast (sorted by movie_id) --------------------------------------
static void ingest_complete_cast(const Ctx& c) {
    std::string tdir = c.out_dir + "/complete_cast"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/complete_cast.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, subject_id, status_id;
    while (p.next()) {
        if (p.fields.size() < 4) { fprintf(stderr,"complete_cast bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        subject_id.push_back(parse_int_or_null(p.fields[2]));
        status_id.push_back(parse_int_or_null(p.fields[3]));
    }
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",         reorder_int(id, perm));
    write_int32(tdir, "movie_id",   reorder_int(movie_id, perm));
    write_int32(tdir, "subject_id", reorder_int(subject_id, perm));
    write_int32(tdir, "status_id",  reorder_int(status_id, perm));
    logmsg("complete_cast: %zu rows", id.size());
}

// ---- movie_companies (sorted by movie_id) ------------------------------------
static void ingest_movie_companies(const Ctx& c) {
    std::string tdir = c.out_dir + "/movie_companies"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/movie_companies.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, company_id, company_type_id;
    VarlenCol note_c;
    while (p.next()) {
        if (p.fields.size() < 5) { fprintf(stderr,"movie_companies bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        company_id.push_back(parse_int_or_null(p.fields[2]));
        company_type_id.push_back(parse_int_or_null(p.fields[3]));
        note_c.append(p.fields[4]);
    }
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",              reorder_int(id, perm));
    write_int32(tdir, "movie_id",        reorder_int(movie_id, perm));
    write_int32(tdir, "company_id",      reorder_int(company_id, perm));
    write_int32(tdir, "company_type_id", reorder_int(company_type_id, perm));
    note_c.reorder(perm).write(tdir, "note");
    logmsg("movie_companies: %zu rows", id.size());
}

// ---- movie_info (sorted by movie_id, 14M rows) -------------------------------
static void ingest_movie_info(const Ctx& c) {
    constexpr size_t N = 14835720;
    std::string tdir = c.out_dir + "/movie_info"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/movie_info.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, info_type_id;
    id.reserve(N); movie_id.reserve(N); info_type_id.reserve(N);
    VarlenCol info_c(N, 32ull * N), note_c(N, 8ull * N);
    while (p.next()) {
        if (p.fields.size() < 5) { fprintf(stderr,"movie_info bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        info_type_id.push_back(parse_int_or_null(p.fields[2]));
        info_c.append(p.fields[3]);
        note_c.append(p.fields[4]);
    }
    logmsg("movie_info: parsed %zu rows", id.size());
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",            reorder_int(id, perm));
    write_int32(tdir, "movie_id",      reorder_int(movie_id, perm));
    write_int32(tdir, "info_type_id",  reorder_int(info_type_id, perm));
    info_c.reorder(perm).write(tdir, "info");
    note_c.reorder(perm).write(tdir, "note");
}

// ---- movie_info_idx (sorted by movie_id) -------------------------------------
static void ingest_movie_info_idx(const Ctx& c) {
    std::string tdir = c.out_dir + "/movie_info_idx"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/movie_info_idx.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, info_type_id;
    VarlenCol info_c, note_c;
    while (p.next()) {
        if (p.fields.size() < 5) { fprintf(stderr,"movie_info_idx bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        info_type_id.push_back(parse_int_or_null(p.fields[2]));
        info_c.append(p.fields[3]);
        note_c.append(p.fields[4]);
    }
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",           reorder_int(id, perm));
    write_int32(tdir, "movie_id",     reorder_int(movie_id, perm));
    write_int32(tdir, "info_type_id", reorder_int(info_type_id, perm));
    info_c.reorder(perm).write(tdir, "info");
    note_c.reorder(perm).write(tdir, "note");
    logmsg("movie_info_idx: %zu rows", id.size());
}

// ---- movie_keyword (sorted by movie_id) --------------------------------------
static void ingest_movie_keyword(const Ctx& c) {
    std::string tdir = c.out_dir + "/movie_keyword"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/movie_keyword.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, keyword_id;
    while (p.next()) {
        if (p.fields.size() < 3) { fprintf(stderr,"movie_keyword bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        keyword_id.push_back(parse_int_or_null(p.fields[2]));
    }
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",         reorder_int(id, perm));
    write_int32(tdir, "movie_id",   reorder_int(movie_id, perm));
    write_int32(tdir, "keyword_id", reorder_int(keyword_id, perm));
    logmsg("movie_keyword: %zu rows", id.size());
}

// ---- movie_link (sorted by movie_id) -----------------------------------------
static void ingest_movie_link(const Ctx& c) {
    std::string tdir = c.out_dir + "/movie_link"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/movie_link.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, movie_id, linked_movie_id, link_type_id;
    while (p.next()) {
        if (p.fields.size() < 4) { fprintf(stderr,"movie_link bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        movie_id.push_back(parse_int_or_null(p.fields[1]));
        linked_movie_id.push_back(parse_int_or_null(p.fields[2]));
        link_type_id.push_back(parse_int_or_null(p.fields[3]));
    }
    auto perm = argsort_int(movie_id);
    write_int32(tdir, "id",              reorder_int(id, perm));
    write_int32(tdir, "movie_id",        reorder_int(movie_id, perm));
    write_int32(tdir, "linked_movie_id", reorder_int(linked_movie_id, perm));
    write_int32(tdir, "link_type_id",    reorder_int(link_type_id, perm));
    logmsg("movie_link: %zu rows", id.size());
}

// ---- person_info (sorted by person_id) ---------------------------------------
static void ingest_person_info(const Ctx& c) {
    std::string tdir = c.out_dir + "/person_info"; mkdirp(tdir);
    auto mf = mmap_file(c.in_dir + "/person_info.csv");
    CsvParser p; p.init(mf.data, mf.size);
    std::vector<int32_t> id, person_id, info_type_id;
    VarlenCol info_c, note_c;
    while (p.next()) {
        if (p.fields.size() < 5) { fprintf(stderr,"person_info bad row %zu\n", p.fields.size()); exit(1); }
        id.push_back(parse_int_or_null(p.fields[0]));
        person_id.push_back(parse_int_or_null(p.fields[1]));
        info_type_id.push_back(parse_int_or_null(p.fields[2]));
        info_c.append(p.fields[3]);
        note_c.append(p.fields[4]);
    }
    auto perm = argsort_int(person_id);
    write_int32(tdir, "id",           reorder_int(id, perm));
    write_int32(tdir, "person_id",    reorder_int(person_id, perm));
    write_int32(tdir, "info_type_id", reorder_int(info_type_id, perm));
    info_c.reorder(perm).write(tdir, "info");
    note_c.reorder(perm).write(tdir, "note");
    logmsg("person_info: %zu rows", id.size());
}

// =============================================================================
// Driver: thread pool, one task per table, ordered by size (big first).
// =============================================================================

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <data_dir> <storage_dir>\n", argv[0]);
        return 1;
    }
    Ctx ctx{ argv[1], argv[2] };
    mkdirp(ctx.out_dir);

    using Task = std::function<void()>;
    std::vector<Task> tasks;

    // Order: largest first so big tables start in parallel
    tasks.push_back([&]{ ingest_cast_info(ctx); });          // 36M
    tasks.push_back([&]{ ingest_movie_info(ctx); });         // 14M
    tasks.push_back([&]{ ingest_movie_keyword(ctx); });      // 4.5M
    tasks.push_back([&]{ ingest_name(ctx); });               // 4.1M
    tasks.push_back([&]{ ingest_char_name(ctx); });          // 3.1M
    tasks.push_back([&]{ ingest_person_info(ctx); });        // 2.9M
    tasks.push_back([&]{ ingest_movie_companies(ctx); });    // 2.6M
    tasks.push_back([&]{ ingest_title(ctx); });              // 2.5M
    tasks.push_back([&]{ ingest_movie_info_idx(ctx); });     // 1.4M
    tasks.push_back([&]{ ingest_aka_name(ctx); });           // 0.9M
    tasks.push_back([&]{ ingest_aka_title(ctx); });          // 0.36M
    tasks.push_back([&]{ ingest_company_name(ctx); });       // 0.23M
    tasks.push_back([&]{ ingest_keyword(ctx); });            // 0.13M
    tasks.push_back([&]{ ingest_complete_cast(ctx); });      // 0.13M
    tasks.push_back([&]{ ingest_movie_link(ctx); });         // 30K
    tasks.push_back([&]{ ingest_small_dim(ctx, "info_type",      "info", 113); });
    tasks.push_back([&]{ ingest_small_dim(ctx, "kind_type",      "kind", 7); });
    tasks.push_back([&]{ ingest_small_dim(ctx, "link_type",      "link", 18); });
    tasks.push_back([&]{ ingest_small_dim(ctx, "role_type",      "role", 12); });
    tasks.push_back([&]{ ingest_small_dim(ctx, "company_type",   "kind", 4); });
    tasks.push_back([&]{ ingest_small_dim(ctx, "comp_cast_type", "kind", 4); });

    std::atomic<size_t> next{0};
    int nthreads = std::min<int>(12, (int)tasks.size());
    std::vector<std::thread> workers;
    for (int t = 0; t < nthreads; ++t) {
        workers.emplace_back([&]{
            while (true) {
                size_t i = next.fetch_add(1);
                if (i >= tasks.size()) return;
                try { tasks[i](); }
                catch (std::exception& e) {
                    fprintf(stderr, "task %zu failed: %s\n", i, e.what()); exit(1);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    logmsg("ingest complete");
    return 0;
}
