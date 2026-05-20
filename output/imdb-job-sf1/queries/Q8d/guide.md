## SQL

```sql
SELECT MIN(an1.name) AS costume_designer_pseudo,
       MIN(t.title) AS movie_with_costumes
FROM aka_name AS an1, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n1, role_type AS rt, title AS t
WHERE cn.country_code ='[us]'
  AND rt.role ='costume designer'
  AND an1.person_id = n1.id  AND n1.id = ci.person_id
  AND ci.movie_id = t.id     AND t.id = mc.movie_id
  AND mc.company_id = cn.id  AND ci.role_id = rt.id
  AND an1.person_id = ci.person_id AND ci.movie_id = mc.movie_id;
```

## Column Reference

### role_type.role (varlen)
Files: `role_type/role.off`, `role_type/role.dat`. Resolve `rt_id` for "costume designer".
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id_cd = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "costume designer") { rt_id_cd = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `us_code`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t us_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[us]") { us_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### cast_info.role_id, movie_id, person_id (int32)
Files: `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`. Length 36244344.

### movie_companies.movie_id, company_id
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. Length 2609129.

### aka_name.person_id, aka_name.name
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}`. Length 901343, sorted by person_id.

### title.title (varlen)
Files: `title/title.off`, `title/title.dat`. Length 2528312. For MIN aggregate.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only; CSR on role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Two equality predicates only (`rt.role='costume designer'`, `cn.country_code='[us]'`). 'costume designer' is rarer than 'writer' → smallish cast_info slice.

Aggregation: MIN(an1.name), MIN(t.title).

Driver (role-driven):
1. Resolve `rt_id_cd` and `us_code`.
2. Use `cast_info__role_id` CSR on `rt_id_cd` → ci row ids.
3. For each ci row r:
   - `pid=ci.person_id[r]`. Require `aka_name__person_id` range non-empty.
   - `mv=ci.movie_id[r]`. Use `movie_companies__movie_id[mv..mv+1]`; check any mc row has `cn.country_code[mc.company_id-1]==us_code`.
   - If both join sides have at least one match, update MINs:
     - MIN(t.title) at title row `mv-1`.
     - MIN(an1.name) over aka_name range for `pid`.

Selectivities:
- 'costume designer' role_id → moderate slice of 36M cast_info (small role).
- '[us]' country_code → high (most cn rows).

LIKE notes: none.

## Indexes

### cast_info__role_id (CSR)
Files: `_idx/cast_info__role_id__offsets.bin` (int32, 14), `_idx/cast_info__role_id__rowids.bin` (int32, 36244344).
```cpp
auto crid_off = read_vec<int32_t>(store + "/_idx/cast_info__role_id__offsets.bin");
auto crid_row = read_vec<int32_t>(store + "/_idx/cast_info__role_id__rowids.bin");
int32_t lo = crid_off[rt_id_cd], hi = crid_off[rt_id_cd+1];
for (int32_t k = lo; k < hi; ++k) { int32_t r = crid_row[k]; /* ci row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mcmid_off = read_vec<int32_t>(store + "/_idx/movie_companies__movie_id__offsets.bin");
int32_t lo = mcmid_off[mv], hi = mcmid_off[mv+1];
```

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).
```cpp
auto akpid_off = read_vec<int32_t>(store + "/_idx/aka_name__person_id__offsets.bin");
int32_t lo = akpid_off[pid], hi = akpid_off[pid+1];
```
