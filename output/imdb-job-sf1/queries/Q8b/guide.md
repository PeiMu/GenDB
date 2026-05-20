## SQL

```sql
SELECT MIN(an.name) AS acress_pseudonym,
       MIN(t.title) AS japanese_anime_movie
FROM aka_name AS an, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n, role_type AS rt, title AS t
WHERE ci.note ='(voice: English version)'
  AND cn.country_code ='[jp]'
  AND mc.note LIKE '%(Japan)%' AND mc.note NOT LIKE '%(USA)%'
  AND (mc.note LIKE '%(2006)%' OR mc.note LIKE '%(2007)%')
  AND n.name LIKE '%Yo%' AND n.name NOT LIKE '%Yu%'
  AND rt.role ='actress'
  AND t.production_year BETWEEN 2006 AND 2007
  AND (t.title LIKE 'One Piece%' OR t.title LIKE 'Dragon Ball Z%')
  AND an.person_id = n.id AND n.id = ci.person_id
  AND ci.movie_id = t.id  AND t.id = mc.movie_id
  AND mc.company_id = cn.id AND ci.role_id = rt.id
  AND an.person_id = ci.person_id AND ci.movie_id = mc.movie_id;
```

## Column Reference

### role_type.role (varlen)
Files: `role_type/role.off`, `role_type/role.dat`. Resolve `rt_id` for "actress".
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "actress") { rt_id = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `jp_code` for "[jp]".
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t jp_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[jp]") { jp_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### cast_info.note (varlen), role_id, movie_id, person_id (int32)
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`. Length 36244344.

### movie_companies.note (varlen), movie_id, company_id (int32)
Files: `movie_companies/note.{off,dat}`, `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. Length 2609129. Apply 4 LIKE checks via `memmem`.

### name.name (varlen)
Files: `name/name.off`, `name/name.dat`. Scan all 4,167,491 rows once for `%Yo%` AND NOT `%Yu%`.

### aka_name.person_id, name
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}`. Use index per pid.

### title.id, title.title, title.production_year
`title/id.bin` identity; `title/title.{off,dat}`; `title/production_year.bin` int32 length 2528312. `t.production_year BETWEEN 2006 AND 2007` AND title LIKE `'One Piece%'` OR LIKE `'Dragon Ball Z%'` (prefix → `memcmp` against first k bytes).

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Aggregation: MIN(an.name), MIN(t.title) over varlen.

Driver: very tight title filter (`production_year IN {2006,2007}` AND prefix LIKE on two specific titles) → maybe ≤ dozens of titles. Use title as driver.

Approach (movie-driven):
1. Scan `title.production_year.bin` & `title.title.{off,dat}` once → list of `t_id` matching year + prefix LIKE.
2. Resolve `rt_id_actress`, `jp_code`.
3. For each `t_id`:
   a. `movie_companies__movie_id` range → for each mc row check note LIKEs (`%(Japan)%` AND NOT `%(USA)%` AND (`%(2006)%` OR `%(2007)%`)) AND `cn.country_code[mc.company_id-1] == jp_code`. If none, skip movie.
   b. `cast_info__movie_id` range → for each ci row check `role_id==rt_id_actress` AND note equality.
   c. For surviving ci row → pid=ci.person_id. Check name.name[pid-1] LIKE rules.
   d. Check `aka_name__person_id` range non-empty; pull aka name(s) for MIN.

Selectivities:
- title.title LIKE `'One Piece%'` OR LIKE `'Dragon Ball Z%'` AND year 2006-2007 → tens of titles.
- `ci.note='(voice: English version)'` → ~1% of cast_info.
- `cn.country_code='[jp]'` → small.

LIKE notes: `'One Piece%'` is prefix; `'Dragon Ball Z%'` is prefix; rest are `%X%`.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314). cast_info is sorted by movie_id.
```cpp
auto cimid_off = read_vec<int32_t>(store + "/_idx/cast_info__movie_id__offsets.bin");
int32_t lo = cimid_off[t_id], hi = cimid_off[t_id+1];
for (int32_t r = lo; r < hi; ++r) { /* ci row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mcmid_off = read_vec<int32_t>(store + "/_idx/movie_companies__movie_id__offsets.bin");
int32_t lo = mcmid_off[t_id], hi = mcmid_off[t_id+1];
```

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).
```cpp
auto akpid_off = read_vec<int32_t>(store + "/_idx/aka_name__person_id__offsets.bin");
int32_t lo = akpid_off[pid], hi = akpid_off[pid+1];
```
