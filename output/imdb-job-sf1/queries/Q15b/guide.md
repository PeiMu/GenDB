# Q15b Guide

## SQL
```sql
/* Q15b */
-- Q15b
SELECT MIN(mi.info) AS release_date,
       MIN(t.title) AS youtube_movie
FROM aka_title AS at1,
     company_name AS cn,
     company_type AS ct,
     info_type AS it1,
     keyword AS k,
     movie_companies AS mc,
     movie_info AS mi,
     movie_keyword AS mk,
     title AS t
WHERE cn.country_code = '[us]'
  AND cn.name = 'YouTube'
  AND it1.info = 'release dates'
  AND mc.note LIKE '%(200%)%'
  AND mc.note LIKE '%(worldwide)%'
  AND mi.note LIKE '%internet%'
  AND mi.info LIKE 'USA:% 200%'
  AND t.production_year BETWEEN 2005 AND 2010
  AND t.id = at1.movie_id
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND t.id = mc.movie_id
  AND mk.movie_id = mi.movie_id
  AND mk.movie_id = mc.movie_id
  AND mk.movie_id = at1.movie_id
  AND mi.movie_id = mc.movie_id
  AND mi.movie_id = at1.movie_id
  AND mc.movie_id = at1.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `aka_title.movie_id` (int32_t)
- File: `aka_title/movie_id.bin` (361472 rows of int32_t)
- Role: Primary FK on which `aka_title` is physically sorted. Use the primary CSR `indexes/aka_title__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `company_name.country_code` (varlen text)
- Files: `company_name/country_code.offsets.bin` (uint64[234998]) + `company_name/country_code.data.bin`
- Predicate (this query): `cn.country_code = '[us]'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `company_name.id` (int32_t)
- File: `company_name/id.bin` (234997 rows of int32_t)
- Role: Dimension primary key. Use `indexes/company_name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `company_name.name` (varlen text)
- Files: `company_name/name.offsets.bin` (uint64[234998]) + `company_name/name.data.bin`
- Predicate (this query): `cn.name = 'YouTube'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `company_type.id` (int32_t)
- File: `company_type/id.bin` (4 rows of int32_t)
- Role: Dimension primary key. Use `indexes/company_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `info_type.id` (int32_t)
- File: `info_type/id.bin` (113 rows of int32_t)
- Role: Dimension primary key. Use `indexes/info_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `info_type.info` (varlen text)
- Files: `info_type/info.offsets.bin` (uint64[114]) + `info_type/info.data.bin`
- Predicate (this query): `it1.info = 'release dates'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `keyword.id` (int32_t)
- File: `keyword/id.bin` (134170 rows of int32_t)
- Role: Dimension primary key. Use `indexes/keyword__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `movie_companies.company_id` (int32_t)
- File: `movie_companies/company_id.bin` (2609129 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_companies__company_id__offsets.bin` (uint64[max+2]) + `indexes/movie_companies__company_id__rowids.bin` (int32[2609129]). Use it when this column is the more selective join key.

### `movie_companies.company_type_id` (int32_t)
- File: `movie_companies/company_type_id.bin` (2609129 rows of int32_t)
- Role: FK to a dimension (no dedicated index on this column in `movie_companies`); test directly as `company_type_id_bin[r] == target_id`.

### `movie_companies.movie_id` (int32_t)
- File: `movie_companies/movie_id.bin` (2609129 rows of int32_t)
- Role: Primary FK on which `movie_companies` is physically sorted. Use the primary CSR `indexes/movie_companies__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `movie_companies.note` (varlen text)
- Files: `movie_companies/note.offsets.bin` (uint64[2609130]) + `movie_companies/note.data.bin`
- Predicate (this query): `mc.note LIKE '%(200%)%'` → varlen substring/prefix match — scan `note.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`
- Predicate (this query): `mc.note LIKE '%(worldwide)%'` → varlen substring/prefix match — scan `note.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `movie_info.info` (varlen text)
- Files: `movie_info/info.offsets.bin` (uint64[14835721]) + `movie_info/info.data.bin`
- Predicate (this query): `mi.info LIKE 'USA:% 200%'` → varlen substring/prefix match — scan `info.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`
- Projected: `MIN(mi.info)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `movie_info.info_type_id` (int32_t)
- File: `movie_info/info_type_id.bin` (14835720 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_info__info_type_id__offsets.bin` (uint64[max+2]) + `indexes/movie_info__info_type_id__rowids.bin` (int32[14835720]). Use it when this column is the more selective join key.

### `movie_info.movie_id` (int32_t)
- File: `movie_info/movie_id.bin` (14835720 rows of int32_t)
- Role: Primary FK on which `movie_info` is physically sorted. Use the primary CSR `indexes/movie_info__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `movie_info.note` (varlen text)
- Files: `movie_info/note.offsets.bin` (uint64[14835721]) + `movie_info/note.data.bin`
- Predicate (this query): `mi.note LIKE '%internet%'` → varlen substring/prefix match — scan `note.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `movie_keyword.keyword_id` (int32_t)
- File: `movie_keyword/keyword_id.bin` (4523930 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_keyword__keyword_id__offsets.bin` (uint64[max+2]) + `indexes/movie_keyword__keyword_id__rowids.bin` (int32[4523930]). Use it when this column is the more selective join key.

### `movie_keyword.movie_id` (int32_t)
- File: `movie_keyword/movie_id.bin` (4523930 rows of int32_t)
- Role: Primary FK on which `movie_keyword` is physically sorted. Use the primary CSR `indexes/movie_keyword__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `title.id` (int32_t)
- File: `title/id.bin` (2528312 rows of int32_t)
- Role: Dimension primary key. Use `indexes/title__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `title.production_year` (int32_t (nullable, -1))
- File: `title/production_year.bin` (2528312 rows of int32_t)
- Predicate (this query): `t.production_year BETWEEN 2005 AND 2010` → int32 range — `lo <= production_year_bin[r] && production_year_bin[r] <= hi`

### `title.title` (varlen text)
- Files: `title/title.offsets.bin` (uint64[2528313]) + `title/title.data.bin`
- Projected: `MIN(t.title)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| aka_title | 361,472 | fact | movie_id | 50000 |
| company_name | 234,997 | dimension(PK) | id | 50000 |
| company_type | 4 | dimension(PK) | id | 4 |
| info_type | 113 | dimension(PK) | id | 113 |
| keyword | 134,170 | dimension(PK) | id | 50000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `title.id` = `aka_title.movie_id`
- `title.id` = `movie_info.movie_id`
- `title.id` = `movie_keyword.movie_id`
- `title.id` = `movie_companies.movie_id`
- `movie_keyword.movie_id` = `movie_info.movie_id`
- `movie_keyword.movie_id` = `movie_companies.movie_id`
- `movie_keyword.movie_id` = `aka_title.movie_id`
- `movie_info.movie_id` = `movie_companies.movie_id`
- `movie_info.movie_id` = `aka_title.movie_id`
- `movie_companies.movie_id` = `aka_title.movie_id`
- `keyword.id` = `movie_keyword.keyword_id`
- `info_type.id` = `movie_info.info_type_id`
- `company_name.id` = `movie_companies.company_id`
- `company_type.id` = `movie_companies.company_type_id`

### Filters (alias.col → predicate)
- `company_name.country_code`: `cn.country_code = '[us]'`
- `company_name.name`: `cn.name = 'YouTube'`
- `info_type.info`: `it1.info = 'release dates'`
- `movie_companies.note`: `mc.note LIKE '%(200%)%'`
- `movie_companies.note`: `mc.note LIKE '%(worldwide)%'`
- `movie_info.note`: `mi.note LIKE '%internet%'`
- `movie_info.info`: `mi.info LIKE 'USA:% 200%'`
- `title.production_year`: `t.production_year BETWEEN 2005 AND 2010`

### Aggregation & projection
- `MIN(mi.info)`, `MIN(t.title)`
- Output is a single row of MIN aggregates (no GROUP BY). Maintain running mins; early-exit is NOT safe (a later row could be lex-smaller).

### Suggested execution outline
1. Resolve each dimension literal to its id by scanning that dimension's text column (use the parallel `id.bin` to read the id of the matching row — do NOT hardcode any id).
2. Drive on `title` (PK 1..2,528,312). For each candidate `t.id = v`, probe every movie-fact primary CSR (`<fact>__movie_id__offsets.bin`) for the range `[off[v], off[v+1])`. Apply per-fact filters inside that range; only then read varlen projections.
3. For dimension-attribute lookups after a fact probe, use the dimension's `indexes/<dim>__id__pos.bin` to convert id → row, then read varlen attributes.

## Indexes Used

### `aka_title__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/aka_title__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `aka_title` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `aka_title/<col>.bin` and `aka_title/<col>.offsets.bin` file.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* aka_title row r */ }`

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

### `movie_keyword__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/movie_keyword__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `movie_keyword` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `movie_keyword/<col>.bin` and `movie_keyword/<col>.offsets.bin` file.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* movie_keyword row r */ }`

### `movie_info__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/movie_info__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `movie_info` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `movie_info/<col>.bin` and `movie_info/<col>.offsets.bin` file.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* movie_info row r */ }`

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

### `movie_keyword__keyword_id` (aux CSR)
- Files:
  - `indexes/movie_keyword__keyword_id__offsets.bin` — `uint64_t[max_keyword_id + 2]` (size = 134172)
  - `indexes/movie_keyword__keyword_id__rowids.bin`  — `int32_t[4523930]`
- Semantics: for key `v`, the matching `movie_keyword` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_keyword`.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_keyword row r */ }`

### `movie_info__info_type_id` (aux CSR)
- Files:
  - `indexes/movie_info__info_type_id__offsets.bin` — `uint64_t[max_info_type_id + 2]` (size = 115)
  - `indexes/movie_info__info_type_id__rowids.bin`  — `int32_t[14835720]`
- Semantics: for key `v`, the matching `movie_info` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `movie_info`.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* movie_info row r */ }`

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

### `keyword__id__pos` (pk_pos_dense)
- File: `indexes/keyword__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `keyword/id.bin`, or `-1` if absent
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

### `company_type__id__pos` (pk_pos_dense)
- File: `indexes/company_type__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `company_type/id.bin`, or `-1` if absent
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
