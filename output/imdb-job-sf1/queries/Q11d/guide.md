# Q11d Guide

## SQL
```sql
/* Q11d */
-- Q11d
SELECT MIN(cn.name) AS from_company,
       MIN(mc.note) AS production_note,
       MIN(t.title) AS movie_based_on_book
FROM company_name AS cn,
     company_type AS ct,
     keyword AS k,
     link_type AS lt,
     movie_companies AS mc,
     movie_keyword AS mk,
     movie_link AS ml,
     title AS t
WHERE cn.country_code !='[pl]'
  AND ct.kind != 'production companies'
  AND ct.kind IS NOT NULL
  AND k.keyword IN ('sequel',
                    'revenge',
                    'based-on-novel')
  AND mc.note IS NOT NULL
  AND t.production_year > 1950
  AND lt.id = ml.link_type_id
  AND ml.movie_id = t.id
  AND t.id = mk.movie_id
  AND mk.keyword_id = k.id
  AND t.id = mc.movie_id
  AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id
  AND ml.movie_id = mk.movie_id
  AND ml.movie_id = mc.movie_id
  AND mk.movie_id = mc.movie_id;
```

## Column Reference

All column files live under `<storage>/<table>/`. Fixed int32 columns are stored as raw little-endian `int32[N]`; nullable variants (`intN` below) use `-1` as the NULL sentinel. Varlen columns use a paired `<col>.offsets.bin` (`uint64[N+1]`) + `<col>.data.bin` (raw bytes); row `i`'s value is `data[off[i]..off[i+1])`, and an empty range means NULL. `char1` columns are raw `uint8[N]` with `0` denoting NULL.

### `company_name.country_code` (varlen text)
- Files: `company_name/country_code.offsets.bin` (uint64[234998]) + `company_name/country_code.data.bin`
- Predicate (this query): `cn.country_code !='[pl]'` → equality test (negated) — `memcmp` slice against literal

### `company_name.id` (int32_t)
- File: `company_name/id.bin` (234997 rows of int32_t)
- Role: Dimension primary key. Use `indexes/company_name__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `company_name.name` (varlen text)
- Files: `company_name/name.offsets.bin` (uint64[234998]) + `company_name/name.data.bin`
- Projected: `MIN(cn.name)` → read value only for surviving rows; maintain a running min (lexicographic for varlen, arithmetic for int32 skipping -1).

### `company_type.id` (int32_t)
- File: `company_type/id.bin` (4 rows of int32_t)
- Role: Dimension primary key. Use `indexes/company_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `company_type.kind` (varlen text)
- Files: `company_type/kind.offsets.bin` (uint64[5]) + `company_type/kind.data.bin`
- Predicate (this query): `ct.kind != 'production companies'` → equality test (negated) — `memcmp` slice against literal
- Predicate (this query): `ct.kind IS NOT NULL` → non-empty slice: `kind_off[r+1] > kind_off[r]`

### `keyword.id` (int32_t)
- File: `keyword/id.bin` (134170 rows of int32_t)
- Role: Dimension primary key. Use `indexes/keyword__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `keyword.keyword` (varlen text)
- Files: `keyword/keyword.offsets.bin` (uint64[134171]) + `keyword/keyword.data.bin`
- Predicate (this query): `k.keyword IN ('sequel',
                    'revenge',
                    'based-on-novel')` → membership in literal set — `std::unordered_set<std::string>` of literals, lookup via slice

### `link_type.id` (int32_t)
- File: `link_type/id.bin` (18 rows of int32_t)
- Role: Dimension primary key. Use `indexes/link_type__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

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
- Predicate (this query): `mc.note IS NOT NULL` → non-empty slice: `note_off[r+1] > note_off[r]`

### `movie_keyword.keyword_id` (int32_t)
- File: `movie_keyword/keyword_id.bin` (4523930 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_keyword__keyword_id__offsets.bin` (uint64[max+2]) + `indexes/movie_keyword__keyword_id__rowids.bin` (int32[4523930]). Use it when this column is the more selective join key.

### `movie_keyword.movie_id` (int32_t)
- File: `movie_keyword/movie_id.bin` (4523930 rows of int32_t)
- Role: Primary FK on which `movie_keyword` is physically sorted. Use the primary CSR `indexes/movie_keyword__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `movie_link.link_type_id` (int32_t)
- File: `movie_link/link_type_id.bin` (29997 rows of int32_t)
- Role: Secondary FK with aux CSR `indexes/movie_link__link_type_id__offsets.bin` (uint64[max+2]) + `indexes/movie_link__link_type_id__rowids.bin` (int32[29997]). Use it when this column is the more selective join key.

### `movie_link.movie_id` (int32_t)
- File: `movie_link/movie_id.bin` (29997 rows of int32_t)
- Role: Primary FK on which `movie_link` is physically sorted. Use the primary CSR `indexes/movie_link__movie_id__offsets.bin` (uint64[max+2]) to get the contiguous range of rows for a given parent id.

### `title.id` (int32_t)
- File: `title/id.bin` (2528312 rows of int32_t)
- Role: Dimension primary key. Use `indexes/title__id__pos.bin` (int32[max_id+2]) to map id→row position. Slot `pos[id] == -1` means the id is absent.

### `title.production_year` (int32_t (nullable, -1))
- File: `title/production_year.bin` (2528312 rows of int32_t)
- Predicate (this query): `t.production_year > 1950` → `production_year_bin[r] > literal`

## Table Stats

| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_name | 234,997 | dimension(PK) | id | 50000 |
| company_type | 4 | dimension(PK) | id | 4 |
| keyword | 134,170 | dimension(PK) | id | 50000 |
| link_type | 18 | dimension(PK) | id | 18 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_link | 29,997 | fact | movie_id | 30000 |
| title | 2,528,312 | dimension(PK)+driver | id | 100000 |

## Query Analysis

### Join graph
- `link_type.id` = `movie_link.link_type_id`
- `movie_link.movie_id` = `title.id`
- `title.id` = `movie_keyword.movie_id`
- `movie_keyword.keyword_id` = `keyword.id`
- `title.id` = `movie_companies.movie_id`
- `movie_companies.company_type_id` = `company_type.id`
- `movie_companies.company_id` = `company_name.id`
- `movie_link.movie_id` = `movie_keyword.movie_id`
- `movie_link.movie_id` = `movie_companies.movie_id`
- `movie_keyword.movie_id` = `movie_companies.movie_id`

### Filters (alias.col → predicate)
- `company_name.country_code`: `cn.country_code !='[pl]'`
- `company_type.kind`: `ct.kind != 'production companies'`
- `company_type.kind`: `ct.kind IS NOT NULL`
- `keyword.keyword`: `k.keyword IN ('sequel',
                    'revenge',
                    'based-on-novel')`
- `movie_companies.note`: `mc.note IS NOT NULL`
- `title.production_year`: `t.production_year > 1950`

### Aggregation & projection
- `MIN(cn.name)`
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
