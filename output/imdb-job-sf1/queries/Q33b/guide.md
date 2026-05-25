# Q33b Guide

## SQL
```sql
/* Q33b */
-- Q33b
SELECT MIN(cn1.name) AS first_company,
       MIN(cn2.name) AS second_company,
       MIN(mi_idx1.info) AS first_rating,
       MIN(mi_idx2.info) AS second_rating,
       MIN(t1.title) AS first_movie,
       MIN(t2.title) AS second_movie
FROM company_name AS cn1,
     company_name AS cn2,
     info_type AS it1,
     info_type AS it2,
     kind_type AS kt1,
     kind_type AS kt2,
     link_type AS lt,
     movie_companies AS mc1,
     movie_companies AS mc2,
     movie_info_idx AS mi_idx1,
     movie_info_idx AS mi_idx2,
     movie_link AS ml,
     title AS t1,
     title AS t2
WHERE cn1.country_code = '[nl]'
  AND it1.info = 'rating'
  AND it2.info = 'rating'
  AND kt1.kind IN ('tv series')
  AND kt2.kind IN ('tv series')
  AND lt.link LIKE '%follow%'
  AND mi_idx2.info < '3.0'
  AND t2.production_year = 2007
  AND lt.id = ml.link_type_id
  AND t1.id = ml.movie_id
  AND t2.id = ml.linked_movie_id
  AND it1.id = mi_idx1.info_type_id
  AND t1.id = mi_idx1.movie_id
  AND kt1.id = t1.kind_id
  AND cn1.id = mc1.company_id
  AND t1.id = mc1.movie_id
  AND ml.movie_id = mi_idx1.movie_id
  AND ml.movie_id = mc1.movie_id
  AND mi_idx1.movie_id = mc1.movie_id
  AND it2.id = mi_idx2.info_type_id
  AND t2.id = mi_idx2.movie_id
  AND kt2.id = t2.kind_id
  AND cn2.id = mc2.company_id
  AND t2.id = mc2.movie_id
  AND ml.linked_movie_id = mi_idx2.movie_id
  AND ml.linked_movie_id = mc2.movie_id
  AND mi_idx2.movie_id = mc2.movie_id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `company_name.country_code` (varlen text)
- Files: `company_name/country_code.offsets.bin` (uint64[234998]) + `company_name/country_code.data.bin`
- Predicate (this query): `cn1.country_code = '[nl]'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `company_name.id` (int32_t)
- File: `company_name/id.bin` (234997 rows of int32_t)
- Role: Dimension primary key. Use `indexes/company_name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `company_name.name` (varlen text)
- Files: `company_name/name.offsets.bin` (uint64[234998]) + `company_name/name.data.bin`
- Projected: `MIN(cn2.name)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `info_type.id` (int32_t)
- File: `info_type/id.bin` (113 rows of int32_t)
- Role: Dimension primary key. Use `indexes/info_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `info_type.info` (varlen text)
- Files: `info_type/info.offsets.bin` (uint64[114]) + `info_type/info.data.bin`
- Predicate (this query): `it1.info = 'rating'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id
- Predicate (this query): `it2.info = 'rating'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `kind_type.id` (int32_t)
- File: `kind_type/id.bin` (7 rows of int32_t)
- Role: Dimension primary key. Use `indexes/kind_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `kind_type.kind` (varlen text)
- Files: `kind_type/kind.offsets.bin` (uint64[8]) + `kind_type/kind.data.bin`
- Predicate (this query): `kt1.kind IN ('tv series')` → membership in literal set — `std::unordered_set<std::string>` of literals, lookup via slice
- Predicate (this query): `kt2.kind IN ('tv series')` → membership in literal set — `std::unordered_set<std::string>` of literals, lookup via slice

### `link_type.id` (int32_t)
- File: `link_type/id.bin` (18 rows of int32_t)
- Role: Dimension primary key. Use `indexes/link_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `link_type.link` (varlen text)
- Files: `link_type/link.offsets.bin` (uint64[19]) + `link_type/link.data.bin`
- Predicate (this query): `lt.link LIKE '%follow%'` → varlen substring/prefix match — scan `link.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `movie_companies.company_id` (int32_t)
- File: `movie_companies/company_id.bin` (2609129 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_companies__company_id__offsets.bin` (uint64[max+2]) + `indexes/movie_companies__company_id__rowids.bin` (int32[2609129]). Use it when this column is the more selective join key.

### `movie_companies.movie_id` (int32_t)
- File: `movie_companies/movie_id.bin` (2609129 rows of int32_t)
- Role: Primary FK on which `movie_companies` is physically sorted. Use the primary CSR `indexes/movie_companies__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `movie_info_idx.info` (varlen text)
- Files: `movie_info_idx/info.offsets.bin` (uint64[1380036]) + `movie_info_idx/info.data.bin`
- Predicate (this query): `mi_idx2.info < '3.0'` → see SQL
- Projected: `MIN(mi_idx2.info)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `movie_info_idx.info_type_id` (int32_t)
- File: `movie_info_idx/info_type_id.bin` (1380035 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_info_idx__info_type_id__offsets.bin` (uint64[max+2]) + `indexes/movie_info_idx__info_type_id__rowids.bin` (int32[1380035]). Use it when this column is the more selective join key.

### `movie_info_idx.movie_id` (int32_t)
- File: `movie_info_idx/movie_id.bin` (1380035 rows of int32_t)
- Role: Primary FK on which `movie_info_idx` is physically sorted. Use the primary CSR `indexes/movie_info_idx__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `movie_link.link_type_id` (int32_t)
- File: `movie_link/link_type_id.bin` (29997 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_link__link_type_id__offsets.bin` (uint64[max+2]) + `indexes/movie_link__link_type_id__rowids.bin` (int32[29997]). Use it when this column is the more selective join key.

### `movie_link.linked_movie_id` (int32_t)
- File: `movie_link/linked_movie_id.bin` (29997 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_link__linked_movie_id__offsets.bin` (uint64[max+2]) + `indexes/movie_link__linked_movie_id__rowids.bin` (int32[29997]). Use it when this column is the more selective join key.

### `movie_link.movie_id` (int32_t)
- File: `movie_link/movie_id.bin` (29997 rows of int32_t)
- Role: Primary FK on which `movie_link` is physically sorted. Use the primary CSR `indexes/movie_link__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `title.id` (int32_t)
- File: `title/id.bin` (2528312 rows of int32_t)
- Role: Dimension primary key. Use `indexes/title__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `title.kind_id` (int32_t)
- File: `title/kind_id.bin` (2528312 rows of int32_t)

### `title.production_year` (int32_t (nullable, -1))
- File: `title/production_year.bin` (2528312 rows of int32_t)
- Predicate (this query): `t2.production_year = 2007` → `production_year_bin[r] == literal_id` (literal_id resolved by scanning the parent dimension's text column)

### `title.title` (varlen text)
- Files: `title/title.offsets.bin` (uint64[2528313]) + `title/title.data.bin`
- Projected: `MIN(t2.title)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_name | 234,997 | dimension(PK) | id | 50000 |
| info_type | 113 | dimension(PK) | id | 113 |
| kind_type | 7 | dimension(PK) | id | 7 |
| link_type | 18 | dimension(PK) | id | 18 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |
| movie_link | 29,997 | fact | movie_id | 30000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `link_type.id` = `movie_link.link_type_id`
- `title.id` = `movie_link.movie_id`
- `title.id` = `movie_link.linked_movie_id`
- `info_type.id` = `movie_info_idx.info_type_id`
- `title.id` = `movie_info_idx.movie_id`
- `kind_type.id` = `title.kind_id`
- `company_name.id` = `movie_companies.company_id`
- `title.id` = `movie_companies.movie_id`
- `movie_link.movie_id` = `movie_info_idx.movie_id`
- `movie_link.movie_id` = `movie_companies.movie_id`
- `movie_info_idx.movie_id` = `movie_companies.movie_id`
- `info_type.id` = `movie_info_idx.info_type_id`
- `title.id` = `movie_info_idx.movie_id`
- `kind_type.id` = `title.kind_id`
- `company_name.id` = `movie_companies.company_id`
- `title.id` = `movie_companies.movie_id`
- `movie_link.linked_movie_id` = `movie_info_idx.movie_id`
- `movie_link.linked_movie_id` = `movie_companies.movie_id`
- `movie_info_idx.movie_id` = `movie_companies.movie_id`

### Filters (alias.col → predicate)
- `company_name.country_code`: `cn1.country_code = '[nl]'`
- `info_type.info`: `it1.info = 'rating'`
- `info_type.info`: `it2.info = 'rating'`
- `kind_type.kind`: `kt1.kind IN ('tv series')`
- `kind_type.kind`: `kt2.kind IN ('tv series')`
- `link_type.link`: `lt.link LIKE '%follow%'`
- `movie_info_idx.info`: `mi_idx2.info < '3.0'`
- `title.production_year`: `t2.production_year = 2007`

### Aggregation & projection
- `MIN(cn1.name)`, `MIN(cn2.name)`, `MIN(mi_idx1.info)`, `MIN(mi_idx2.info)`, `MIN(t1.title)`, `MIN(t2.title)`
- Output is a single row of MIN aggregates (no GROUP BY). Maintain running mins; early-exit is NOT safe (a later row could be lex-smaller).

### Suggested execution outline
1. Resolve each dimension literal to its id by scanning that dimension's text column (use the parallel `id.bin` to read the id of the matching row — do NOT hardcode any id).
2. Drive on `title` (PK 1..2,528,312). For each candidate `t.id = v`, probe every movie-fact primary CSR (`<fact>__movie_id__offsets.bin`) for the range `[off[v], off[v+1])`. Apply per-fact filters inside that range; only then read varlen projections.
3. For dimension-attribute lookups after a fact probe, use the dimension's `indexes/<dim>__id__pos.bin` to convert id → row, then read varlen attributes.

## Indexes Used

### `movie_link__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/movie_link__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `movie_link` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `movie_link/<col>.bin` and `movie_link/<col>.offsets.bin` file.
- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).
  Negative/NULL `movie_id` values are bucketed at `v=0`.
- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):
  ```cpp
  offsets.assign((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = key[r] < 0 ? 0 : key[r];
      offsets[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* movie_link row r */ }`

### `movie_companies__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/movie_companies__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `movie_companies` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `movie_companies/<col>.bin` and `movie_companies/<col>.offsets.bin` file.
- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).
  Negative/NULL `movie_id` values are bucketed at `v=0`.
- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):
  ```cpp
  offsets.assign((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = key[r] < 0 ? 0 : key[r];
      offsets[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* movie_companies row r */ }`

### `movie_info_idx__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/movie_info_idx__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `movie_info_idx` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `movie_info_idx/<col>.bin` and `movie_info_idx/<col>.offsets.bin` file.
- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).
  Negative/NULL `movie_id` values are bucketed at `v=0`.
- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):
  ```cpp
  offsets.assign((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = key[r] < 0 ? 0 : key[r];
      offsets[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* movie_info_idx row r */ }`

### `movie_link__linked_movie_id` (aux CSR)
- Files:
  - `indexes/movie_link__linked_movie_id__offsets.bin` — `uint64_t[max_linked_movie_id + 2]` (size = 2528314)
  - `indexes/movie_link__linked_movie_id__rowids.bin`  — `int32_t[29997]`
- Semantics: for key `v`, the matching `movie_link` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_link`.
- Sentinel: empty range when `off[v] == off[v+1]`.
- Build code (verbatim from `build_aux` in `build_indexes.cpp`):
  ```cpp
  std::vector<uint64_t> off((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      off[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];
  std::vector<int32_t> rowids(N);
  std::vector<uint64_t> cur = off;
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      rowids[cur[(size_t)k]++] = (int32_t)r;
  }
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_link row r */ }`

### `movie_link__link_type_id` (aux CSR)
- Files:
  - `indexes/movie_link__link_type_id__offsets.bin` — `uint64_t[max_link_type_id + 2]` (size = 20)
  - `indexes/movie_link__link_type_id__rowids.bin`  — `int32_t[29997]`
- Semantics: for key `v`, the matching `movie_link` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_link`.
- Sentinel: empty range when `off[v] == off[v+1]`.
- Build code (verbatim from `build_aux` in `build_indexes.cpp`):
  ```cpp
  std::vector<uint64_t> off((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      off[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];
  std::vector<int32_t> rowids(N);
  std::vector<uint64_t> cur = off;
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      rowids[cur[(size_t)k]++] = (int32_t)r;
  }
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_link row r */ }`

### `movie_companies__company_id` (aux CSR)
- Files:
  - `indexes/movie_companies__company_id__offsets.bin` — `uint64_t[max_company_id + 2]` (size = 234999)
  - `indexes/movie_companies__company_id__rowids.bin`  — `int32_t[2609129]`
- Semantics: for key `v`, the matching `movie_companies` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_companies`.
- Sentinel: empty range when `off[v] == off[v+1]`.
- Build code (verbatim from `build_aux` in `build_indexes.cpp`):
  ```cpp
  std::vector<uint64_t> off((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      off[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];
  std::vector<int32_t> rowids(N);
  std::vector<uint64_t> cur = off;
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      rowids[cur[(size_t)k]++] = (int32_t)r;
  }
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_companies row r */ }`

### `movie_info_idx__info_type_id` (aux CSR)
- Files:
  - `indexes/movie_info_idx__info_type_id__offsets.bin` — `uint64_t[max_info_type_id + 2]` (size = 115)
  - `indexes/movie_info_idx__info_type_id__rowids.bin`  — `int32_t[1380035]`
- Semantics: for key `v`, the matching `movie_info_idx` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_info_idx`.
- Sentinel: empty range when `off[v] == off[v+1]`.
- Build code (verbatim from `build_aux` in `build_indexes.cpp`):
  ```cpp
  std::vector<uint64_t> off((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      off[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < off.size(); ++i) off[i] += off[i-1];
  std::vector<int32_t> rowids(N);
  std::vector<uint64_t> cur = off;
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = keys[r] < 0 ? 0 : keys[r];
      rowids[cur[(size_t)k]++] = (int32_t)r;
  }
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_info_idx row r */ }`

### `title__id__pos` (pk_pos_dense)
- File: `indexes/title__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `title/id.bin`, or `-1` if absent
- Sentinel: `-1` for missing ids
- Build code (verbatim):
  ```cpp
  std::vector<int32_t> pos((size_t)max_id + 2, -1);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t id = ids[r];
      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
  }
  ```
- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`

### `company_name__id__pos` (pk_pos_dense)
- File: `indexes/company_name__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `company_name/id.bin`, or `-1` if absent
- Sentinel: `-1` for missing ids
- Build code (verbatim):
  ```cpp
  std::vector<int32_t> pos((size_t)max_id + 2, -1);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t id = ids[r];
      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
  }
  ```
- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`

### `info_type__id__pos` (pk_pos_dense)
- File: `indexes/info_type__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `info_type/id.bin`, or `-1` if absent
- Sentinel: `-1` for missing ids
- Build code (verbatim):
  ```cpp
  std::vector<int32_t> pos((size_t)max_id + 2, -1);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t id = ids[r];
      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
  }
  ```
- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`

### `link_type__id__pos` (pk_pos_dense)
- File: `indexes/link_type__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `link_type/id.bin`, or `-1` if absent
- Sentinel: `-1` for missing ids
- Build code (verbatim):
  ```cpp
  std::vector<int32_t> pos((size_t)max_id + 2, -1);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t id = ids[r];
      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
  }
  ```
- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`

### `kind_type__id__pos` (pk_pos_dense)
- File: `indexes/kind_type__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `kind_type/id.bin`, or `-1` if absent
- Sentinel: `-1` for missing ids
- Build code (verbatim):
  ```cpp
  std::vector<int32_t> pos((size_t)max_id + 2, -1);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t id = ids[r];
      if (id >= 0 && id <= max_id) pos[(size_t)id] = (int32_t)r;
  }
  ```
- Probe: `int32_t row = pos[id]; if (row < 0) /* not present */;`

## Dimension Literal Resolution

Every equality on a dimension text column (e.g., `it.info = 'rating'`, `ct.kind = 'production companies'`) must be resolved at query time by scanning that dimension's varlen column and reading the parallel `id.bin` at the matching row. NEVER hardcode a dimension id constant — the value depends on the data load.

```cpp
// Generic dimension lookup template
uint64_t Nd = *(uint64_t*)mmap_bytes("<dim>/__row_count.bin");
const uint64_t* doff = (const uint64_t*)mmap_bytes("<dim>/<text_col>.offsets.bin");
const char*     ddat =                  mmap_bytes("<dim>/<text_col>.data.bin");
const int32_t*  dids = (const int32_t*) mmap_bytes("<dim>/id.bin");
int32_t target_id = -1;
for (uint64_t r = 0; r < Nd; ++r) {
    std::string_view s(ddat + doff[r], doff[r+1] - doff[r]);
    if (s == LITERAL) { target_id = dids[r]; break; }
}
```

Then use `indexes/<dim>__id__pos.bin` to map any later id-from-fact back to a row position for reading other dimension attributes.

## Sentinels & Null Handling
- int32 nullable (`intN`): `-1`
- char1 nullable: `0`
- varlen NULL: empty range (`off[i] == off[i+1]`)
- pk_pos missing id: `-1`
- CSR empty bucket: `off[v] == off[v+1]`
