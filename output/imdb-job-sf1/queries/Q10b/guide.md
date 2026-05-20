## SQL

```sql
SELECT MIN(chn.name) AS uncredited_voiced_character,
       MIN(t.title)  AS russian_mov_with_actor_producer
FROM char_name AS chn, cast_info AS ci, company_name AS cn, company_type AS ct,
     movie_companies AS mc, role_type AS rt, title AS t
WHERE ci.note LIKE '%(producer)%'
  AND cn.country_code = '[ru]'
  AND rt.role = 'actor'
  AND t.production_year > 2010
  AND t.id = mc.movie_id AND t.id = ci.movie_id
  AND ci.movie_id = mc.movie_id
  AND chn.id = ci.person_role_id
  AND rt.id = ci.role_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

## Column Reference

### role_type.role (varlen) → `rt_id_actor`
Files: `role_type/role.off`, `role_type/role.dat`.
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id_actor = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "actor") { rt_id_actor = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict) → `ru_code`
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t ru_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[ru]") { ru_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### cast_info.note (varlen), role_id, movie_id, person_role_id
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_role_id.bin`. LIKE `%(producer)%` via `memmem`.

### movie_companies.movie_id, company_id, company_type_id
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`.

### company_type.id (identity)
File: `company_type/id.bin`. No filter on ct — purely structural join.

### title.title, title.production_year
Files: `title/title.{off,dat}`, `title/production_year.bin`. `> 2010`.

### char_name.name (varlen)
File: `char_name/name.{off,dat}`. Dense-PK row chn_id-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_type | 4 | id | dense-PK; no predicate |
| company_name | 234,997 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id; CSR on company_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Aggregation: MIN(chn.name), MIN(t.title).

Driver: russian companies → russian-related movies → small. Same shape as Q10a but `ci.note LIKE '%(producer)%'` (single substring) and `production_year > 2010`.

Approach (cn → mc → t → ci, movie-driven):
1. Resolve `rt_id_actor`, `ru_code`.
2. Collect `ru_company_ids = { cid : cn.country_code[cid-1] == ru_code }`.
3. For each `cid` in `ru_company_ids`, walk `movie_companies__company_id` CSR → mc rows → collect candidate movies `mv`.
4. Dedupe `mv`s. For each `mv`:
   - `title.production_year[mv-1] > 2010`.
   - `cast_info__movie_id[mv..mv+1]`: ci row r with `role_id == rt_id_actor`, note contains `(producer)`, `person_role_id != INT32_MIN`.
   - chn_id = ci.person_role_id[r]; update MIN(chn.name) at row chn_id-1; MIN(t.title) at title row mv-1.

Selectivities:
- `cn.country_code='[ru]'` → small (~few hundred companies).
- Year > 2010 → small slice of titles.
- `rt.role='actor'` → 1 of 12.
- `ci.note LIKE '%(producer)%'` → small. Note: ci.note is mostly null for non-special cast; producer notes are uncommon.

LIKE notes: one substring; `memmem` once on note slice.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto cimid_off = read_vec<int32_t>(store + "/_idx/cast_info__movie_id__offsets.bin");
int32_t lo = cimid_off[mv], hi = cimid_off[mv+1];
```

### movie_companies__company_id (CSR)
Files: `_idx/movie_companies__company_id__offsets.bin` (int32, 234999), `_idx/movie_companies__company_id__rowids.bin` (int32, 2609129).
```cpp
auto mccid_off = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__offsets.bin");
auto mccid_row = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__rowids.bin");
int32_t lo = mccid_off[cid], hi = mccid_off[cid+1];
for (int32_t k = lo; k < hi; ++k) { int32_t r = mccid_row[k]; int32_t mv = mc_movie_id[r]; }
```

### char_name lookup
Direct deref `char_name.name[chn_id-1]`; no index file.
