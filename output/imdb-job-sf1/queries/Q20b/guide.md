# Q20b Guide

## SQL
```sql
/* Q20b */
-- Q20b
SELECT MIN(t.title) AS complete_downey_ironman_movie
FROM complete_cast AS cc,
     comp_cast_type AS cct1,
     comp_cast_type AS cct2,
     char_name AS chn,
     cast_info AS ci,
     keyword AS k,
     kind_type AS kt,
     movie_keyword AS mk,
     name AS n,
     title AS t
WHERE cct1.kind = 'cast'
  AND cct2.kind LIKE '%complete%'
  AND chn.name NOT LIKE '%Sherlock%'
  AND (chn.name LIKE '%Tony%Stark%'
       OR chn.name LIKE '%Iron%Man%')
  AND k.keyword IN ('superhero',
                    'sequel',
                    'second-part',
                    'marvel-comics',
                    'based-on-comic',
                    'tv-special',
                    'fight',
                    'violence')
  AND kt.kind = 'movie'
  AND n.name LIKE '%Downey%Robert%'
  AND t.production_year > 2000
  AND kt.id = t.kind_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND t.id = cc.movie_id
  AND mk.movie_id = ci.movie_id
  AND mk.movie_id = cc.movie_id
  AND ci.movie_id = cc.movie_id
  AND chn.id = ci.person_role_id
  AND n.id = ci.person_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `cast_info.movie_id` (int32_t)
- File: `cast_info/movie_id.bin` (36244344 rows of int32_t)
- Role: Primary FK on which `cast_info` is physically sorted. Use the primary CSR `indexes/cast_info__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `cast_info.person_id` (int32_t)
- File: `cast_info/person_id.bin` (36244344 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/cast_info__person_id__offsets.bin` (uint64[max+2]) + `indexes/cast_info__person_id__rowids.bin` (int32[36244344]). Use it when this column is the more selective join key.

### `cast_info.person_role_id` (int32_t (nullable, -1))
- File: `cast_info/person_role_id.bin` (36244344 rows of int32_t)
- Role: FK to a dimension (no dedicated index on this column in `cast_info`); test directly as `person_role_id_bin[r] == target_id`.

### `char_name.id` (int32_t)
- File: `char_name/id.bin` (3140339 rows of int32_t)
- Role: Dimension primary key. Use `indexes/char_name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `char_name.name` (varlen text)
- Files: `char_name/name.offsets.bin` (uint64[3140340]) + `char_name/name.data.bin`
- Predicate (this query): `chn.name NOT LIKE '%Sherlock%'` → varlen substring/prefix match — scan `name.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`
- Predicate (this query): `(chn.name LIKE '%Tony%Stark%'
       OR chn.name LIKE '%Iron%Man%')` → varlen substring/prefix match — scan `name.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `comp_cast_type.id` (int32_t)
- File: `comp_cast_type/id.bin` (4 rows of int32_t)
- Role: Dimension primary key. Use `indexes/comp_cast_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `comp_cast_type.kind` (varlen text)
- Files: `comp_cast_type/kind.offsets.bin` (uint64[5]) + `comp_cast_type/kind.data.bin`
- Predicate (this query): `cct1.kind = 'cast'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id
- Predicate (this query): `cct2.kind LIKE '%complete%'` → varlen substring/prefix match — scan `kind.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `complete_cast.movie_id` (int32_t (nullable, -1))
- File: `complete_cast/movie_id.bin` (135086 rows of int32_t)
- Role: Primary FK on which `complete_cast` is physically sorted. Use the primary CSR `indexes/complete_cast__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `complete_cast.status_id` (int32_t)
- File: `complete_cast/status_id.bin` (135086 rows of int32_t)
- Role: FK to a dimension (no dedicated index on this column in `complete_cast`); test directly as `status_id_bin[r] == target_id`.

### `complete_cast.subject_id` (int32_t)
- File: `complete_cast/subject_id.bin` (135086 rows of int32_t)
- Role: FK to a dimension (no dedicated index on this column in `complete_cast`); test directly as `subject_id_bin[r] == target_id`.

### `keyword.id` (int32_t)
- File: `keyword/id.bin` (134170 rows of int32_t)
- Role: Dimension primary key. Use `indexes/keyword__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `keyword.keyword` (varlen text)
- Files: `keyword/keyword.offsets.bin` (uint64[134171]) + `keyword/keyword.data.bin`
- Predicate (this query): `k.keyword IN ('superhero',
                    'sequel',
                    'second-part',
                    'marvel-comics',
                    'based-on-comic',
                    'tv-special',
                    'fight',
                    'violence')` → membership in literal set — `std::unordered_set<std::string>` of literals, lookup via slice

### `kind_type.id` (int32_t)
- File: `kind_type/id.bin` (7 rows of int32_t)
- Role: Dimension primary key. Use `indexes/kind_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `kind_type.kind` (varlen text)
- Files: `kind_type/kind.offsets.bin` (uint64[8]) + `kind_type/kind.data.bin`
- Predicate (this query): `kt.kind = 'movie'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `movie_keyword.keyword_id` (int32_t)
- File: `movie_keyword/keyword_id.bin` (4523930 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_keyword__keyword_id__offsets.bin` (uint64[max+2]) + `indexes/movie_keyword__keyword_id__rowids.bin` (int32[4523930]). Use it when this column is the more selective join key.

### `movie_keyword.movie_id` (int32_t)
- File: `movie_keyword/movie_id.bin` (4523930 rows of int32_t)
- Role: Primary FK on which `movie_keyword` is physically sorted. Use the primary CSR `indexes/movie_keyword__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `name.id` (int32_t)
- File: `name/id.bin` (4167491 rows of int32_t)
- Role: Dimension primary key. Use `indexes/name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `name.name` (varlen text)
- Files: `name/name.offsets.bin` (uint64[4167492]) + `name/name.data.bin`
- Predicate (this query): `n.name LIKE '%Downey%Robert%'` → varlen substring/prefix match — scan `name.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `title.id` (int32_t)
- File: `title/id.bin` (2528312 rows of int32_t)
- Role: Dimension primary key. Use `indexes/title__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `title.kind_id` (int32_t)
- File: `title/kind_id.bin` (2528312 rows of int32_t)

### `title.production_year` (int32_t (nullable, -1))
- File: `title/production_year.bin` (2528312 rows of int32_t)
- Predicate (this query): `t.production_year > 2000` → `production_year_bin[r] > literal`

### `title.title` (varlen text)
- Files: `title/title.offsets.bin` (uint64[2528313]) + `title/title.data.bin`
- Projected: `MIN(t.title)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| cast_info | 36,244,344 | fact | movie_id | 500000 |
| char_name | 3,140,339 | dimension(PK) | id | 100000 |
| comp_cast_type | 4 | dimension(PK) | id | 4 |
| complete_cast | 135,086 | fact | movie_id | 50000 |
| keyword | 134,170 | dimension(PK) | id | 50000 |
| kind_type | 7 | dimension(PK) | id | 7 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| name | 4,167,491 | dimension(PK) | id | 100000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `kind_type.id` = `title.kind_id`
- `title.id` = `movie_keyword.movie_id`
- `title.id` = `cast_info.movie_id`
- `title.id` = `complete_cast.movie_id`
- `movie_keyword.movie_id` = `cast_info.movie_id`
- `movie_keyword.movie_id` = `complete_cast.movie_id`
- `cast_info.movie_id` = `complete_cast.movie_id`
- `char_name.id` = `cast_info.person_role_id`
- `name.id` = `cast_info.person_id`
- `keyword.id` = `movie_keyword.keyword_id`
- `comp_cast_type.id` = `complete_cast.subject_id`
- `comp_cast_type.id` = `complete_cast.status_id`

### Filters (alias.col → predicate)
- `comp_cast_type.kind`: `cct1.kind = 'cast'`
- `comp_cast_type.kind`: `cct2.kind LIKE '%complete%'`
- `char_name.name`: `chn.name NOT LIKE '%Sherlock%'`
- `char_name.name`: `(chn.name LIKE '%Tony%Stark%'
       OR chn.name LIKE '%Iron%Man%')`
- `keyword.keyword`: `k.keyword IN ('superhero',
                    'sequel',
                    'second-part',
                    'marvel-comics',
                    'based-on-comic',
                    'tv-special',
                    'fight',
                    'violence')`
- `kind_type.kind`: `kt.kind = 'movie'`
- `name.name`: `n.name LIKE '%Downey%Robert%'`
- `title.production_year`: `t.production_year > 2000`

### Aggregation & projection
- `MIN(t.title)`
- Output is a single row of MIN aggregates (no GROUP BY). Maintain running mins; early-exit is NOT safe (a later row could be lex-smaller).

### Suggested execution outline
1. Resolve each dimension literal to its id by scanning that dimension's text column (use the parallel `id.bin` to read the id of the matching row — do NOT hardcode any id).
2. Drive on `title` (PK 1..2,528,312). For each candidate `t.id = v`, probe every movie-fact primary CSR (`<fact>__movie_id__offsets.bin`) for the range `[off[v], off[v+1])`. Apply per-fact filters inside that range; only then read varlen projections.
3. For dimension-attribute lookups after a fact probe, use the dimension's `indexes/<dim>__id__pos.bin` to convert id → row, then read varlen attributes.

## Indexes Used

### `complete_cast__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/complete_cast__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `complete_cast` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `complete_cast/<col>.bin` and `complete_cast/<col>.offsets.bin` file.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* complete_cast row r */ }`

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

### `cast_info__movie_id` (primary CSR — table physically sorted by movie_id)
- File: `indexes/cast_info__movie_id__offsets.bin`
- Layout: `uint64_t[max_movie_id + 2]` (size = 2528314)
- Semantics: `cast_info` rows are stored contiguously sorted by `movie_id`.
  Rows with `movie_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `cast_info/<col>.bin` and `cast_info/<col>.offsets.bin` file.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* cast_info row r */ }`

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

### `cast_info__person_id` (aux CSR)
- Files:
  - `indexes/cast_info__person_id__offsets.bin` — `uint64_t[max_person_id + 2]` (size = 4167493)
  - `indexes/cast_info__person_id__rowids.bin`  — `int32_t[36244344]`
- Semantics: for key `v`, the matching `cast_info` row positions are
  `rowids[off[v] .. off[v+1])`. Each rowid indexes into the column files of `cast_info`.
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
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t k = lo; k < hi; ++k) { int32_t r = rowids[k]; /* cast_info row r */ }`

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

### `name__id__pos` (pk_pos_dense)
- File: `indexes/name__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `name/id.bin`, or `-1` if absent
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

### `char_name__id__pos` (pk_pos_dense)
- File: `indexes/char_name__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `char_name/id.bin`, or `-1` if absent
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

### `comp_cast_type__id__pos` (pk_pos_dense)
- File: `indexes/comp_cast_type__id__pos.bin`
- Layout: `int32_t[max_id + 2]` (built by `build_pk_pos` in `build_indexes.cpp`)
- Semantics: `pos[id]` = row position of that id in `comp_cast_type/id.bin`, or `-1` if absent
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
