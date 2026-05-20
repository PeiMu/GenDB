## SQL

```sql
SELECT MIN(chn.name) AS uncredited_voiced_character,
       MIN(t.title)  AS movie_with_american_producer
FROM char_name AS chn, cast_info AS ci, company_name AS cn, company_type AS ct,
     movie_companies AS mc, role_type AS rt, title AS t
WHERE ci.note LIKE '%(producer)%'
  AND cn.country_code = '[us]'
  AND t.production_year > 1990
  AND t.id = mc.movie_id AND t.id = ci.movie_id
  AND ci.movie_id = mc.movie_id
  AND chn.id = ci.person_role_id
  AND rt.id = ci.role_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

NOTE: Q10c has NO `rt.role` predicate (rt is structurally joined but unconstrained).

## Column Reference

### role_type (no predicate)
Files: `role_type/id.bin` (identity, length 12). `rt.id = ci.role_id` is a non-filtering join — every ci row with non-null role_id satisfies it. Skip rt scan.

### company_name.country_code (int16 dict) → `us_code`
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t us_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[us]") { us_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### cast_info.note (varlen), movie_id, person_role_id
Files: `cast_info/note.{off,dat}`, `cast_info/movie_id.bin`, `cast_info/person_role_id.bin`. LIKE `%(producer)%` via `memmem`.

### movie_companies.movie_id, company_id, company_type_id
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`.

### company_type.id (identity)
File: `company_type/id.bin`. No predicate.

### title.title, title.production_year
Files: `title/title.{off,dat}`, `title/production_year.bin`. `> 1990` (NULL = INT32_MIN, excluded).

### char_name.name (varlen)
File: `char_name/name.{off,dat}`. Dense-PK row chn_id-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK; no filter |
| company_type | 4 | id | dense-PK; no filter |
| company_name | 234,997 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id; CSR on company_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Aggregation: MIN(chn.name), MIN(t.title).

`cn.country_code='[us]'` is broad (majority of company_name) — driving from cn is poor. `ci.note LIKE '%(producer)%'` is the most selective filter here.

Approach (note-driven scan of cast_info):
1. Resolve `us_code`.
2. Single pass over cast_info (sorted by movie_id):
   - For each ci row r with `person_role_id != INT32_MIN` AND note contains `(producer)`:
     - `mv = ci.movie_id[r]`. Check `title.production_year[mv-1] > 1990`.
     - `movie_companies__movie_id[mv..mv+1]`: any mc with `cn.country_code[mc.company_id-1] == us_code`. (ct join is structural.)
     - chn_id = ci.person_role_id[r]; update MIN(chn.name) at row chn_id-1; MIN(t.title) at title row mv-1.

Memoize per-movie qualifier so repeat ci rows for same mv reuse the mc lookup.

Selectivities:
- `ci.note LIKE '%(producer)%'` → small fraction of 36M.
- `production_year > 1990` → ~3/4 of titles.
- `cn.country_code='[us]'` → broad.

LIKE notes: one substring; `memmem` once on note slice. Prune by length: needle is 10 bytes.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto cimid_off = read_vec<int32_t>(store + "/_idx/cast_info__movie_id__offsets.bin");
int32_t lo = cimid_off[mv], hi = cimid_off[mv+1];
```
(Used either to gate on mv or simply iterate ci linearly — both paths valid since cast_info is sorted by movie_id.)

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mcmid_off = read_vec<int32_t>(store + "/_idx/movie_companies__movie_id__offsets.bin");
int32_t lo = mcmid_off[mv], hi = mcmid_off[mv+1];
```

### char_name lookup
Direct deref `char_name.name[chn_id-1]`; no index file.
