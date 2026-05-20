## SQL

```sql
SELECT MIN(an.name) AS alternative_name,
       MIN(chn.name) AS voiced_character_name,
       MIN(n.name)   AS voicing_actress,
       MIN(t.title)  AS american_movie
FROM aka_name AS an, char_name AS chn, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n, role_type AS rt, title AS t
WHERE ci.note IN ('(voice)','(voice: Japanese version)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code ='[us]'
  AND n.gender ='f' AND n.name LIKE '%An%'
  AND rt.role ='actress'
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

### name.name (varlen) — LIKE `%An%`
Files: `name/name.off`, `name/name.dat`. `%An%` is very common — large candidate set.

### cast_info.note (varlen), role_id, movie_id, person_id, person_role_id
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin`. IN check via `flat_hash_set<string_view>`.

### movie_companies.movie_id, company_id
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. NO note filter in Q9c.

### title.title
File: `title/title.{off,dat}`. NO production_year filter in Q9c.

### aka_name.person_id, aka_name.name
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}`.

### char_name.name
File: `char_name/name.{off,dat}`. Dense-PK lookup at row chn_id-1.

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

Aggregation: MIN(an.name), MIN(chn.name), MIN(n.name), MIN(t.title). Loosest variant of Q9.

Driver (person-driven still best — character/voice filter narrows downstream):
1. Resolve constants `rt_id_actress`, `us_code`, `f_code`.
2. Scan `name`: collect `pid` where `gender==f_code` AND `name` LIKE `%An%`. `%An%` matches a huge fraction; could be hundreds of thousands.
3. Pre-filter on aka_name (range non-empty under `aka_name__person_id`).
4. For each surviving pid, walk `cast_info__person_id`; filter `role_id==rt_id_actress`, note in voice set, person_role_id != INT32_MIN.
5. For each (pid, ci row r, mv=ci.movie_id[r]): walk `movie_companies__movie_id` range and check any mc with `cn.country_code[mc.company_id-1] == us_code` (no note filter).
6. chn_id = ci.person_role_id[r]; update MINs.

Selectivities:
- `n.gender='f'` ≈ 1/3. `%An%` matches huge subset (10–20%). Combined → hundreds of thousands.
- IN-set on ci.note → still small fraction of 36M.
- `cn.country_code='[us]'` → broad.
- No year, no mc.note filter → looser than Q9a/b.

LIKE notes: only `%An%`.

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
Direct deref `char_name.name[chn_id-1]`; no index file.
