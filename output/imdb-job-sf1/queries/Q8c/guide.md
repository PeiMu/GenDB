## SQL

```sql
SELECT MIN(a1.name) AS writer_pseudo_name,
       MIN(t.title) AS movie_title
FROM aka_name AS a1, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n1, role_type AS rt, title AS t
WHERE cn.country_code ='[us]'
  AND rt.role ='writer'
  AND a1.person_id = n1.id   AND n1.id = ci.person_id
  AND ci.movie_id = t.id     AND t.id = mc.movie_id
  AND mc.company_id = cn.id  AND ci.role_id = rt.id
  AND a1.person_id = ci.person_id AND ci.movie_id = mc.movie_id;
```

## Column Reference

### role_type.role (varlen)
Files: `role_type/role.off`, `role_type/role.dat`. Resolve `rt_id` for "writer".
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id_writer = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "writer") { rt_id_writer = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `us_code` for "[us]".
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

### movie_companies.movie_id, company_id (int32)
Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. Length 2609129.

### name.id (identity), aka_name.person_id, aka_name.name
Files: `name/id.bin` (identity, length 4167491). `aka_name/person_id.bin`, `aka_name/name.{off,dat}`.

### title.title (varlen)
Files: `title/title.off`, `title/title.dat`. For MIN aggregate; length 2528312.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Very loose filters — only two equality predicates (`rt.role='writer'`, `cn.country_code='[us]'`). Result will be enormous to enumerate; but MIN(a1.name) and MIN(t.title) over varlen → still cheap if we drive carefully.

Aggregation: MIN(a1.name), MIN(t.title).

Driver (role-driven, smallest known):
1. Resolve `rt_id_writer` (1 row of 12 role_types). Resolve `us_code`.
2. Use `cast_info__role_id` CSR to enumerate all ci rows with role_id=writer.
3. For each such ci row r:
   - `pid = ci.person_id[r]`. Check `aka_name__person_id` range non-empty (else discard for an1 join).
   - `mv = ci.movie_id[r]`. Walk `movie_companies__movie_id[mv..mv+1]`; for each mc row check `cn.country_code[mc.company_id-1] == us_code`.
   - If movie qualifies, update `MIN(t.title)` via `title/title.{off,dat}` at row `mv-1`, and `MIN(a1.name)` over aka_name range.

Selectivities:
- `rt.role='writer'` → cast_info rows with role_id=writer ≈ a few million (1/12 of 36M but skewed).
- `cn.country_code='[us]'` → fraction of 235K company_names → fraction of 2.6M mc rows.
- No row-level person filter → use aka_name existence test only.

LIKE notes: none.

## Indexes

### cast_info__role_id (CSR)
Files: `_idx/cast_info__role_id__offsets.bin` (int32, 14), `_idx/cast_info__role_id__rowids.bin` (int32, 36244344). parent_max_id=12.
```cpp
auto crid_off = read_vec<int32_t>(store + "/_idx/cast_info__role_id__offsets.bin");
auto crid_row = read_vec<int32_t>(store + "/_idx/cast_info__role_id__rowids.bin");
int32_t lo = crid_off[rt_id_writer], hi = crid_off[rt_id_writer+1];
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
