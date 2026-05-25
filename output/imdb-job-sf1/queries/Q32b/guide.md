# Q32b Guide

## SQL
```sql
/* Q32b */
-- Q32b
SELECT MIN(lt.link) AS link_type,
       MIN(t1.title) AS first_movie,
       MIN(t2.title) AS second_movie
FROM keyword AS k,
     link_type AS lt,
     movie_keyword AS mk,
     movie_link AS ml,
     title AS t1,
     title AS t2
WHERE k.keyword ='character-name-in-title'
  AND mk.keyword_id = k.id
  AND t1.id = mk.movie_id
  AND ml.movie_id = t1.id
  AND ml.linked_movie_id = t2.id
  AND lt.id = ml.link_type_id
  AND mk.movie_id = t1.id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `keyword.id` (int32_t)
- File: `keyword/id.bin` (134170 rows of int32_t)
- Role: Dimension primary key. Use `indexes/keyword__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `keyword.keyword` (varlen text)
- Files: `keyword/keyword.offsets.bin` (uint64[134171]) + `keyword/keyword.data.bin`
- Predicate (this query): `k.keyword ='character-name-in-title'` → equality — resolve literal id once (see dimension PK scan); test FK column directly if comparing to dim id

### `link_type.id` (int32_t)
- File: `link_type/id.bin` (18 rows of int32_t)
- Role: Dimension primary key. Use `indexes/link_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `link_type.link` (varlen text)
- Files: `link_type/link.offsets.bin` (uint64[19]) + `link_type/link.data.bin`
- Projected: `MIN(lt.link)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `movie_keyword.keyword_id` (int32_t)
- File: `movie_keyword/keyword_id.bin` (4523930 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_keyword__keyword_id__offsets.bin` (uint64[max+2]) + `indexes/movie_keyword__keyword_id__rowids.bin` (int32[4523930]). Use it when this column is the more selective join key.

### `movie_keyword.movie_id` (int32_t)
- File: `movie_keyword/movie_id.bin` (4523930 rows of int32_t)
- Role: Primary FK on which `movie_keyword` is physically sorted. Use the primary CSR `indexes/movie_keyword__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

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

### `title.title` (varlen text)
- Files: `title/title.offsets.bin` (uint64[2528313]) + `title/title.data.bin`
- Projected: `MIN(t1.title)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| keyword | 134,170 | dimension(PK) | id | 50000 |
| link_type | 18 | dimension(PK) | id | 18 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_link | 29,997 | fact | movie_id | 30000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `movie_keyword.keyword_id` = `keyword.id`
- `title.id` = `movie_keyword.movie_id`
- `movie_link.movie_id` = `title.id`
- `movie_link.linked_movie_id` = `title.id`
- `link_type.id` = `movie_link.link_type_id`
- `movie_keyword.movie_id` = `title.id`

### Filters (alias.col → predicate)
- `keyword.keyword`: `k.keyword ='character-name-in-title'`

### Aggregation & projection
- `MIN(lt.link)`, `MIN(t1.title)`, `MIN(t2.title)`
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
