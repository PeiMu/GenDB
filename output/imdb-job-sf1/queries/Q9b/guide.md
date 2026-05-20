## SQL

```sql
SELECT MIN(an.name) AS alternative_name,
       MIN(chn.name) AS voiced_character,
       MIN(n.name)   AS voicing_actress,
       MIN(t.title)  AS american_movie
FROM aka_name AS an, char_name AS chn, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n, role_type AS rt, title AS t
WHERE ci.note = '(voice)'
  AND cn.country_code ='[us]'
  AND mc.note LIKE '%(200%)%'
  AND (mc.note LIKE '%(USA)%' OR mc.note LIKE '%(worldwide)%')
  AND n.gender ='f' AND n.name LIKE '%Angel%'
  AND rt.role ='actress'
  AND t.production_year BETWEEN 2007 AND 2010
  AND ci.movie_id = t.id AND t.id = mc.movie_id
  AND ci.movie_id = mc.movie_id
  AND mc.company_id = cn.id AND ci.role_id = rt.id
  AND n.id = ci.person_id   AND chn.id = ci.person_role_id
  AND an.person_id = n.id   AND an.person_id = ci.person_id;
```

## Column Reference

### role_type.role (varlen) → `rt_id_actress`
Files: `role_type/role.off`, `role_type/role.dat`.
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id_actress = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "actress") { rt_id_actress = (int32_t)(i+1); break; }
```

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

### name.gender (int8 dict) → `f_code`
Files: `name/gender.bin`, `name/gender.dict.off`, `name/gender.dict.dat`.
```cpp
auto g_off = read_vec<int64_t>(store + "/name/gender.dict.off");
std::string g_dat = read_file(store + "/name/gender.dict.dat");
int8_t f_code = 0;
for (size_t i = 0; i+1 < g_off.size(); ++i)
    if (std::string_view(g_dat.data()+g_off[i], g_off[i+1]-g_off[i]) == "f") { f_code = (int8_t)(i+1); break; }
auto n_gender = read_vec<int8_t>(store + "/name/gender.bin");
```

### name.name (varlen) — LIKE `%Angel%`
Files: `name/name.off`, `name/name.dat`. Scan once filtering gender==f_code.

### cast_info.note (varlen), role_id, movie_id, person_id, person_role_id
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin`. Equality compare ci.note == '(voice)'.

### movie_companies.note, movie_id, company_id
Files: `movie_companies/note.{off,dat}`, `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. LIKE `%(200%)%` AND (`%(USA)%` OR `%(worldwide)%`).

### title.title, title.production_year
Files: `title/title.{off,dat}`, `title/production_year.bin`. BETWEEN 2007 AND 2010.

### aka_name.person_id, aka_name.name
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}`. Sorted by person_id.

### char_name.id, char_name.name
Files: `char_name/id.bin` (identity), `char_name/name.{off,dat}`. Dense-PK row = id-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only; CSR on person_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Aggregation: MIN(an.name), MIN(chn.name), MIN(n.name), MIN(t.title).

Tighter than Q9a: `n.name LIKE '%Angel%'` (rarer than '%Ang%'), `ci.note = '(voice)'` equality (smallest of the IN list), 4-year window 2007-2010.

Driver (person-driven):
1. Resolve `rt_id_actress`, `us_code`, `f_code`.
2. Scan `name` once: collect `pid` where gender code==f_code AND name LIKE `%Angel%`. Expected: low hundreds.
3. For each pid:
   - aka_name range non-empty.
   - `cast_info__person_id` CSR → ci row r: role_id==rt_id_actress, note == '(voice)' (length 7 then memcmp), person_role_id != INT32_MIN.
   - `mv = ci.movie_id[r]`; check `title.production_year[mv-1]` in [2007,2010].
   - `movie_companies__movie_id[mv..mv+1]` → any mc with cn.country_code[mc.company_id-1]==us_code AND note LIKE `%(200%)%` AND (`%(USA)%` OR `%(worldwide)%`).
   - chn_id = ci.person_role_id[r]; update MIN(chn.name) at char_name row chn_id-1.
   - Update MIN(an.name) over aka_name range; MIN(n.name) at name row pid-1; MIN(t.title) at title row mv-1.

Selectivities: very tight; expect handful of results.

LIKE notes: `%Angel%` substring; `%(200%)%` matches any year 2000-2099 substring in note text. `%(USA)%`, `%(worldwide)%` substrings.

## Indexes

### cast_info__person_id (CSR)
Files: `_idx/cast_info__person_id__offsets.bin` (int32, 4167493), `_idx/cast_info__person_id__rowids.bin` (int32, 36244344).
```cpp
auto cipid_off = read_vec<int32_t>(store + "/_idx/cast_info__person_id__offsets.bin");
auto cipid_row = read_vec<int32_t>(store + "/_idx/cast_info__person_id__rowids.bin");
int32_t lo = cipid_off[pid], hi = cipid_off[pid+1];
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).

### char_name lookup
Dense-PK direct deref at `char_name.name[chn_id-1]`; no index file.
