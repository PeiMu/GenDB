# Shared Storage Context for IMDB-JOB SF1 Guides

This file gives Phase-2 agents the verbatim facts they need to write query guides.
Use these conventions exactly when authoring per-query guides.

## Storage Root
`<storage>/` contains one subdirectory per table plus `<storage>/_idx/` for indexes.

## Column file layouts
- **int32_t column** `<col>`: file `<table>/<col>.bin`, raw little-endian int32 array
  of length `rows`. NULL encoded as `INT32_MIN` (constant `NULL_INT`).
- **int8_t column** (dict code) `<col>`: file `<table>/<col>.bin`, int8 array length
  `rows`. Code `0` = NULL. Codes `1..K` index dictionary entries `0..K-1`.
- **int16_t column** (dict code) `<col>`: file `<table>/<col>.bin`, int16 array length
  `rows`. Code `0` = NULL. Codes `1..K` index dictionary entries `0..K-1`.
- **varlen column** `<col>`: two files
  - `<table>/<col>.off`: int64 array length `rows+1`, byte offsets into `.dat`
  - `<table>/<col>.dat`: raw bytes, no terminators
  - Row i string = bytes `[off[i], off[i+1])`. Empty string represents NULL.
- **dict-encoded column** `<col>`: three files
  - `<table>/<col>.bin`: int8 or int16 code array (see column type)
  - `<table>/<col>.dict.off`: int64 array length `K+1`
  - `<table>/<col>.dict.dat`: raw bytes
  - Dictionary entry `i` (zero-based) = bytes `[dict.off[i], dict.off[i+1])`
  - Code `c` in the column references dictionary entry `c-1`; code `0` = NULL.

## Dense-PK tables (id_is_dense_1_to_N = true)
For these tables a row with `id = v` lives at array index `v - 1`. Tables:
`title`, `name`, `char_name`, `keyword`, `company_name`, `company_type`,
`comp_cast_type`, `info_type`, `kind_type`, `link_type`, `role_type`.
The `id.bin` file is `[1, 2, ..., MAXID]` (identity).

## FK-sorted tables (sort_order = movie_id or person_id)
- `aka_name` (sorted by person_id, 901343 rows)
- `aka_title` (sorted by movie_id, 361472 rows)
- `cast_info` (sorted by movie_id, 36244344 rows)
- `complete_cast` (sorted by movie_id, 135086 rows)
- `movie_companies` (sorted by movie_id, 2609129 rows)
- `movie_info` (sorted by movie_id, 14835720 rows)
- `movie_info_idx` (sorted by movie_id, 1380035 rows)
- `movie_keyword` (sorted by movie_id, 4523930 rows)
- `movie_link` (sorted by movie_id, 29997 rows)
- `person_info` (sorted by person_id, 2963664 rows)

## Indexes (from build_indexes.cpp — authoritative)

### offsets_only index
File: `<storage>/_idx/<child>__<fkcol>__offsets.bin`
Layout: `int32` array of size `max_parent_id + 2`.
Semantics: rows in `<child>` whose `<fkcol>` equals parent id `v` live in
the contiguous range `[off[v], off[v+1])` of the child's columnar arrays
(because child is sorted by `<fkcol>`). Slot `0` holds the count of rows
with `fkcol < 1` (NULLs / unknowns). Slot `max_parent_id + 1` is one past
the last valid range.

C++ build code (verbatim from build_indexes.cpp):
```cpp
std::vector<int32_t> off(maxid + 2, (int32_t)fk.size());
int32_t i = 0;
for (int32_t v = 0; v <= maxid + 1; ++v) {
    while (i < (int32_t)fk.size() && fk[i] < v) ++i;
    off[v] = i;
}
```

Query-time usage:
```cpp
int32_t lo = off[parent_id];
int32_t hi = off[parent_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
```

All offsets_only indexes (parent → child):
| Index name | Child | FK col | parent_max_id |
|---|---|---|---|
| `cast_info__movie_id` | cast_info | movie_id | 2528312 |
| `movie_info__movie_id` | movie_info | movie_id | 2528312 |
| `movie_info_idx__movie_id` | movie_info_idx | movie_id | 2528312 |
| `movie_keyword__movie_id` | movie_keyword | movie_id | 2528312 |
| `movie_companies__movie_id` | movie_companies | movie_id | 2528312 |
| `movie_link__movie_id` | movie_link | movie_id | 2528312 |
| `complete_cast__movie_id` | complete_cast | movie_id | 2528312 |
| `aka_title__movie_id` | aka_title | movie_id | 2528312 |
| `aka_name__person_id` | aka_name | person_id | 4167491 |
| `person_info__person_id` | person_info | person_id | 4167491 |

### CSR index
Two files:
- `<storage>/_idx/<child>__<fkcol>__offsets.bin`: int32, size `max_parent_id + 2`
- `<storage>/_idx/<child>__<fkcol>__rowids.bin`: int32, size `N_child`

Semantics: for parent id `v`, child row IDs with `<fkcol> = v` live in
`rowids[off[v] .. off[v+1])`. Each rowid is an index into the child's
columnar arrays.

C++ build code (verbatim from build_indexes.cpp):
```cpp
std::vector<int32_t> count(maxid + 2, 0);
for (size_t i = 0; i < N; ++i) {
    int32_t v = fk[i];
    if (v < 0) v = 0;
    if (v > maxid) v = maxid + 1;
    count[v]++;
}
std::vector<int32_t> off(maxid + 2, 0);
int32_t running = 0;
for (int32_t v = 0; v <= maxid + 1; ++v) {
    off[v] = running;
    running += count[v];
}
std::vector<int32_t> rowids(N);
std::vector<int32_t> cursor = off;
for (int32_t i = 0; i < (int32_t)N; ++i) {
    int32_t v = fk[i];
    if (v < 0) v = 0;
    if (v > maxid) v = maxid + 1;
    rowids[cursor[v]++] = i;
}
```

Query-time usage:
```cpp
int32_t lo = off[parent_id];
int32_t hi = off[parent_id + 1];
for (int32_t k = lo; k < hi; ++k) {
    int32_t child_row = rowids[k];
    /* read child columns at index child_row */
}
```

All CSR indexes:
| Index name | Child | FK col | parent_max_id | N_child |
|---|---|---|---|---|
| `cast_info__person_id` | cast_info | person_id | 4167491 | 36244344 |
| `cast_info__person_role_id` | cast_info | person_role_id | 3140339 | 36244344 |
| `cast_info__role_id` | cast_info | role_id | 12 | 36244344 |
| `movie_keyword__keyword_id` | movie_keyword | keyword_id | 134170 | 4523930 |
| `movie_info__info_type_id` | movie_info | info_type_id | 113 | 14835720 |
| `movie_info_idx__info_type_id` | movie_info_idx | info_type_id | 113 | 1380035 |
| `person_info__info_type_id` | person_info | info_type_id | 113 | 2963664 |
| `movie_companies__company_id` | movie_companies | company_id | 234997 | 2609129 |
| `movie_companies__company_type_id` | movie_companies | company_type_id | 4 | 2609129 |
| `movie_link__link_type_id` | movie_link | link_type_id | 18 | 29997 |
| `movie_link__linked_movie_id` | movie_link | linked_movie_id | 2528312 | 29997 |
| `complete_cast__subject_id` | complete_cast | subject_id | 4 | 135086 |
| `complete_cast__status_id` | complete_cast | status_id | 4 | 135086 |
| `title__kind_id` | title | kind_id | 7 | 2528312 |

## Dict-encoded columns (load pattern)
There are two dict-encoded columns in the schema:
- `name.gender` (int8 codes, very small dictionary — `m`, `f`, NULL)
- `company_name.country_code` (int16 codes, ~hundreds of country codes)

At query time, load the dictionary and resolve the codes for the literal(s) used:
```cpp
// Load dict bytes + offsets
auto off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string dat = read_file(store + "/company_name/country_code.dict.dat");
// Find code for a literal (e.g. "[us]")
int16_t target_code = 0;  // 0 means literal not found / NULL
for (size_t i = 0; i + 1 < off.size(); ++i) {
    std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
    if (s == "[us]") { target_code = (int16_t)(i + 1); break; }
}
// Then compare column codes directly: country_code_bin[row] == target_code
```
Never hardcode the integer code; always resolve from dictionary at runtime.

## Column-id ↔ row-index identity
For each dense-PK dimension (`info_type`, `kind_type`, `link_type`, `role_type`,
`company_type`, `comp_cast_type`, `keyword`, `company_name`, `char_name`,
`name`, `title`), to find the id whose text column matches a literal, scan
the varlen column and return `i + 1` (the id) once a match is found:

```cpp
auto off = read_vec<int64_t>(store + "/info_type/info.off");
std::string dat = read_file(store + "/info_type/info.dat");
int32_t target_id = 0;
for (size_t i = 0; i + 1 < off.size(); ++i) {
    std::string_view s(dat.data() + off[i], off[i+1] - off[i]);
    if (s == "rating") { target_id = (int32_t)(i + 1); break; }
}
```

## Sentinels
- Integer NULL: `INT32_MIN` (a.k.a. `NULL_INT`).
- Dict-code NULL: `0`.
- Varlen NULL: zero-length entry (`off[i] == off[i+1]`).
- Index offsets: `off[0]` is the count of rows with FK < 1 (NULLs / out-of-range);
  real parents start at `off[1]`.

## Selectivity hints
- Equality filter on small dimension text → 1 matching id, used directly.
- `LIKE '%...%'` requires a full varlen scan (use `memmem`/two-pointer).
- `BETWEEN year1 AND year2` and `> year` on `production_year` → scan int32 array
  of length 2528312 once.
- `IN (...)` for varlen `mi.info` (14M rows) → build a `flat_hash_set<string_view>`
  of literals, scan rows, prune by length first.
