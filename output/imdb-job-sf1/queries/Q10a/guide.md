## SQL

```sql
SELECT MIN(chn.name) AS uncredited_voiced_character,
       MIN(t.title)  AS russian_movie
FROM char_name AS chn, cast_info AS ci, company_name AS cn, company_type AS ct,
     movie_companies AS mc, role_type AS rt, title AS t
WHERE ci.note LIKE '%(voice)%' AND ci.note LIKE '%(uncredited)%'
  AND cn.country_code = '[ru]'
  AND rt.role = 'actor'
  AND t.production_year > 2005
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
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_role_id.bin`. Both LIKE filters via `memmem`. `person_role_id` nullable (INT32_MIN).
```cpp
auto ci_note_off = read_vec<int64_t>(store + "/cast_info/note.off");
std::string ci_note_dat = read_file(store + "/cast_info/note.dat");
auto note_has = [&](int32_t r, const char* needle, size_t nlen){
    int64_t a=ci_note_off[r], b=ci_note_off[r+1];
    return memmem(ci_note_dat.data()+a, b-a, needle, nlen) != nullptr;
};
```

### movie_companies.movie_id, company_id, company_type_id
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`. Length 2609129.

### company_type.id (identity)
File: `company_type/id.bin`. Dense-PK length 4. `ct.id = mc.company_type_id` is unconstrained (no filter on company_type) → just propagates; row exists for any valid `company_type_id`.

### title.title, title.production_year
Files: `title/title.{off,dat}`, `title/production_year.bin` (int32, 2528312). Filter `> 2005` (NULL = INT32_MIN < 2005 so excluded).

### char_name.name (varlen)
File: `char_name/name.{off,dat}`. Dense-PK; row chn_id-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_type | 4 | id | dense-PK; no predicate |
| company_name | 234,997 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph:
```
rt --role_id-- ci --movie_id-- t --movie_id-- mc --company_id-- cn
                |                                 \-- company_type_id -- ct
                +--person_role_id-- chn (via ci.person_role_id deref into char_name dense PK)
```

Aggregation: MIN(chn.name), MIN(t.title).

Driver: title is selective (production_year > 2005 → roughly half of titles, ~1.2M). Better: role-driven on `actor` (1 of 12) AND/then movies with russian companies. Russia is rare (~1% of companies) → drive from mc by scanning cn.

Approach (movie-driven via cn → mc):
1. Resolve `rt_id_actor`, `ru_code`.
2. Build set of `company_id` where `cn.country_code[cid-1] == ru_code` (scan 235K int16).
3. Scan `movie_companies`: for each mc row where `company_id ∈ ru_set` collect `mv = mc.movie_id`. (No mc.note filter.) → small set of russian-related movies.
4. For each such `mv`:
   - Check `title.production_year[mv-1] > 2005`.
   - `cast_info__movie_id[mv..mv+1]`: for each ci row r:
     - `role_id == rt_id_actor`; ci.note has both `(voice)` AND `(uncredited)`; `person_role_id != INT32_MIN`.
     - chn_id = ci.person_role_id[r]; update MIN(chn.name) at char_name row chn_id-1; MIN(t.title) at title row mv-1.

Selectivities:
- `cn.country_code='[ru]'` → small (few hundred companies → few k mc rows).
- `production_year > 2005` → ~half titles.
- `rt.role='actor'` → 1 of 12 role_types.
- `ci.note LIKE '%(voice)%' AND '%(uncredited)%'` → very small slice.

LIKE notes: both are substring; `memmem` twice on each note slice. Order checks: do shorter needle first.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314). cast_info sorted by movie_id.
```cpp
auto cimid_off = read_vec<int32_t>(store + "/_idx/cast_info__movie_id__offsets.bin");
int32_t lo = cimid_off[mv], hi = cimid_off[mv+1];
for (int32_t r = lo; r < hi; ++r) { /* ci row r */ }
```

### movie_companies__company_id (CSR) — alternative entry path from cn
Files: `_idx/movie_companies__company_id__offsets.bin` (int32, 234999), `_idx/movie_companies__company_id__rowids.bin` (int32, 2609129).
```cpp
auto mccid_off = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__offsets.bin");
auto mccid_row = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__rowids.bin");
int32_t lo = mccid_off[cid], hi = mccid_off[cid+1];
for (int32_t k = lo; k < hi; ++k) { int32_t r = mccid_row[k]; int32_t mv = mc_movie_id[r]; }
```
This avoids scanning all 2.6M mc rows when iterating russian `cid`s.

### char_name lookup
Direct deref `char_name.name[chn_id-1]`; no index file.
