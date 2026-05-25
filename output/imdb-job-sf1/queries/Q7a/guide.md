# Q7a Guide

## SQL
```sql
/* Q7a */
-- Q7a
SELECT MIN(n.name) AS of_person,
       MIN(t.title) AS biography_movie
FROM aka_name AS an,
     cast_info AS ci,
     info_type AS it,
     link_type AS lt,
     movie_link AS ml,
     name AS n,
     person_info AS pi,
     title AS t
WHERE an.name LIKE '%a%'
  AND it.info ='mini biography'
  AND lt.link ='features'
  AND n.name_pcode_cf BETWEEN 'A' AND 'F'
  AND (n.gender='m'
       OR (n.gender = 'f'
           AND n.name LIKE 'B%'))
  AND pi.note ='Volker Boehm'
  AND t.production_year BETWEEN 1980 AND 1995
  AND n.id = an.person_id
  AND n.id = pi.person_id
  AND ci.person_id = n.id
  AND t.id = ci.movie_id
  AND ml.linked_movie_id = t.id
  AND lt.id = ml.link_type_id
  AND it.id = pi.info_type_id
  AND pi.person_id = an.person_id
  AND pi.person_id = ci.person_id
  AND an.person_id = ci.person_id
  AND ci.movie_id = ml.linked_movie_id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `aka_name.name` (varlen text)
- Files: `aka_name/name.offsets.bin` (uint64[901344]) + `aka_name/name.data.bin`
- Predicate (this query): `an.name LIKE '%a%'` → varlen substring/prefix match — scan `name.data.bin` using `memmem`/`memcmp` on slice `[off[r], off[r+1])`

### `aka_name.person_id` (int32_t)
- File: `aka_name/person_id.bin` (901343 rows of int32_t)
- Role: Primary FK on which `aka_name` is physically sorted. Use the primary CSR `indexes/aka_name__person_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `cast_info.movie_id` (int32_t)
- File: `cast_info/movie_id.bin` (36244344 rows of int32_t)
- Role: Primary FK on which `cast_info` is physically sorted. Use the primary CSR `indexes/cast_info__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `cast_info.person_id` (int32_t)
- File: `cast_info/person_id.bin` (36244344 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/cast_info__person_id__offsets.bin` (uint64[max+2]) + `indexes/cast_info__person_id__rowids.bin` (int32[36244344]). Use it when this column is the more selective join key.

### `info_type.id` (int32_t)
- File: `info_type/id.bin` (113 rows of int32_t)
- Role: Dimension primary key. Use `indexes/info_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `info_type.info` (varlen text)
- Files: `info_type/info.offsets.bin` (uint64[114]) + `info_type/info.data.bin`
- Predicate (this query): `it.info ='mini biography'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `link_type.id` (int32_t)
- File: `link_type/id.bin` (18 rows of int32_t)
- Role: Dimension primary key. Use `indexes/link_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `link_type.link` (varlen text)
- Files: `link_type/link.offsets.bin` (uint64[19]) + `link_type/link.data.bin`
- Predicate (this query): `lt.link ='features'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `movie_link.link_type_id` (int32_t)
- File: `movie_link/link_type_id.bin` (29997 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_link__link_type_id__offsets.bin` (uint64[max+2]) + `indexes/movie_link__link_type_id__rowids.bin` (int32[29997]). Use it when this column is the more selective join key.

### `movie_link.linked_movie_id` (int32_t)
- File: `movie_link/linked_movie_id.bin` (29997 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_link__linked_movie_id__offsets.bin` (uint64[max+2]) + `indexes/movie_link__linked_movie_id__rowids.bin` (int32[29997]). Use it when this column is the more selective join key.

### `name.gender` (uint8_t)
- File: `name/gender.bin` (4167491 rows of uint8_t)
- Predicate (this query): `(n.gender='m'
       OR (n.gender = 'f'
           AND n.name LIKE 'B%'))` → `gender_bin[r] == (uint8_t)literal_char` (e.g., 'f' = 0x66)

### `name.id` (int32_t)
- File: `name/id.bin` (4167491 rows of int32_t)
- Role: Dimension primary key. Use `indexes/name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `name.name` (varlen text)
- Files: `name/name.offsets.bin` (uint64[4167492]) + `name/name.data.bin`
- Projected: `MIN(n.name)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `name.name_pcode_cf` (varlen text)
- Files: `name/name_pcode_cf.offsets.bin` (uint64[4167492]) + `name/name_pcode_cf.data.bin`
- Predicate (this query): `n.name_pcode_cf BETWEEN 'A' AND 'F'` → varlen lexicographic range — compare slice against literal bounds

### `person_info.info_type_id` (int32_t)
- File: `person_info/info_type_id.bin` (2963664 rows of int32_t)
- Role: FK to a dimension (no dedicated index on this column in `person_info`); test directly as `info_type_id_bin[r] == target_id`.

### `person_info.note` (varlen text)
- Files: `person_info/note.offsets.bin` (uint64[2963665]) + `person_info/note.data.bin`
- Predicate (this query): `pi.note ='Volker Boehm'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `person_info.person_id` (int32_t)
- File: `person_info/person_id.bin` (2963664 rows of int32_t)
- Role: Primary FK on which `person_info` is physically sorted. Use the primary CSR `indexes/person_info__person_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `title.id` (int32_t)
- File: `title/id.bin` (2528312 rows of int32_t)
- Role: Dimension primary key. Use `indexes/title__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `title.production_year` (int32_t (nullable, -1))
- File: `title/production_year.bin` (2528312 rows of int32_t)
- Predicate (this query): `t.production_year BETWEEN 1980 AND 1995` → int32 range — `lo <= production_year_bin[r] && production_year_bin[r] <= hi`

### `title.title` (varlen text)
- Files: `title/title.offsets.bin` (uint64[2528313]) + `title/title.data.bin`
- Projected: `MIN(t.title)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| aka_name | 901,343 | fact | person_id | 50000 |
| cast_info | 36,244,344 | fact | movie_id | 500000 |
| info_type | 113 | dimension(PK) | id | 113 |
| link_type | 18 | dimension(PK) | id | 18 |
| movie_link | 29,997 | fact | movie_id | 30000 |
| name | 4,167,491 | dimension(PK) | id | 100000 |
| person_info | 2,963,664 | fact | person_id | 100000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `name.id` = `aka_name.person_id`
- `name.id` = `person_info.person_id`
- `cast_info.person_id` = `name.id`
- `title.id` = `cast_info.movie_id`
- `movie_link.linked_movie_id` = `title.id`
- `link_type.id` = `movie_link.link_type_id`
- `info_type.id` = `person_info.info_type_id`
- `person_info.person_id` = `aka_name.person_id`
- `person_info.person_id` = `cast_info.person_id`
- `aka_name.person_id` = `cast_info.person_id`
- `cast_info.movie_id` = `movie_link.linked_movie_id`

### Filters (alias.col → predicate)
- `aka_name.name`: `an.name LIKE '%a%'`
- `info_type.info`: `it.info ='mini biography'`
- `link_type.link`: `lt.link ='features'`
- `name.name_pcode_cf`: `n.name_pcode_cf BETWEEN 'A' AND 'F'`
- `name.gender`: `(n.gender='m'
       OR (n.gender = 'f'
           AND n.name LIKE 'B%'))`
- `person_info.note`: `pi.note ='Volker Boehm'`
- `title.production_year`: `t.production_year BETWEEN 1980 AND 1995`

### Aggregation & projection
- `MIN(n.name)`, `MIN(t.title)`
- Output is a single row of MIN aggregates (no GROUP BY). Maintain running mins; early-exit is NOT safe (a later row could be lex-smaller).

### Suggested execution outline
1. Resolve each dimension literal to its id by scanning that dimension's text column (use the parallel `id.bin` to read the id of the matching row — do NOT hardcode any id).
2. Drive on `title` (PK 1..2,528,312). For each candidate `t.id = v`, probe every movie-fact primary CSR (`<fact>__movie_id__offsets.bin`) for the range `[off[v], off[v+1])`. Apply per-fact filters inside that range; only then read varlen projections.
3. For dimension-attribute lookups after a fact probe, use the dimension's `indexes/<dim>__id__pos.bin` to convert id → row, then read varlen attributes.

## Indexes Used

### `aka_name__person_id` (primary CSR — table physically sorted by person_id)
- File: `indexes/aka_name__person_id__offsets.bin`
- Layout: `uint64_t[max_person_id + 2]` (size = 4167493)
- Semantics: `aka_name` rows are stored contiguously sorted by `person_id`.
  Rows with `person_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `aka_name/<col>.bin` and `aka_name/<col>.offsets.bin` file.
- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).
  Negative/NULL `person_id` values are bucketed at `v=0`.
- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):
  ```cpp
  offsets.assign((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = key[r] < 0 ? 0 : key[r];
      offsets[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* aka_name row r */ }`

### `person_info__person_id` (primary CSR — table physically sorted by person_id)
- File: `indexes/person_info__person_id__offsets.bin`
- Layout: `uint64_t[max_person_id + 2]` (size = 4167493)
- Semantics: `person_info` rows are stored contiguously sorted by `person_id`.
  Rows with `person_id = v` occupy contiguous positions `[off[v], off[v+1])`
  in every `person_info/<col>.bin` and `person_info/<col>.offsets.bin` file.
- Sentinel: empty range when `off[v] == off[v+1]` (no rows for that key).
  Negative/NULL `person_id` values are bucketed at `v=0`.
- Build code (verbatim from `counting_sort` in `build_indexes.cpp`):
  ```cpp
  offsets.assign((size_t)max_k + 2, 0);
  for (uint64_t r = 0; r < N; ++r) {
      int32_t k = key[r] < 0 ? 0 : key[r];
      offsets[(size_t)k + 1]++;
  }
  for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
  ```
- Probe: `uint64_t lo = off[v], hi = off[v+1]; for (uint64_t r = lo; r < hi; ++r) { /* person_info row r */ }`

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
