## SQL

```sql
SELECT MIN(an.name) AS alternative_name,
       MIN(chn.name) AS character_name,
       MIN(t.title) AS movie
FROM aka_name AS an, char_name AS chn, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n, role_type AS rt, title AS t
WHERE ci.note IN ('(voice)','(voice: Japanese version)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code ='[us]'
  AND mc.note IS NOT NULL
  AND (mc.note LIKE '%(USA)%' OR mc.note LIKE '%(worldwide)%')
  AND n.gender ='f'
  AND n.name LIKE '%Ang%'
  AND rt.role ='actress'
  AND t.production_year BETWEEN 2005 AND 2015
  AND ci.movie_id = t.id  AND t.id = mc.movie_id
  AND ci.movie_id = mc.movie_id
  AND mc.company_id = cn.id  AND ci.role_id = rt.id
  AND n.id = ci.person_id   AND chn.id = ci.person_role_id
  AND an.person_id = n.id   AND an.person_id = ci.person_id;
```

## Column Reference

### role_type.role (varlen) → resolve `rt_id_actress`
Files: `role_type/role.off`, `role_type/role.dat`.
```cpp
auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
std::string rt_dat = read_file(store + "/role_type/role.dat");
int32_t rt_id_actress = 0;
for (size_t i = 0; i+1 < rt_off.size(); ++i)
    if (std::string_view(rt_dat.data()+rt_off[i], rt_off[i+1]-rt_off[i]) == "actress") { rt_id_actress = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict) → resolve `us_code`
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t us_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[us]") { us_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### name.gender (int8 dict) → resolve `f_code`
Files: `name/gender.bin` (int8, length 4167491), `name/gender.dict.off`, `name/gender.dict.dat`.
```cpp
auto g_off = read_vec<int64_t>(store + "/name/gender.dict.off");
std::string g_dat = read_file(store + "/name/gender.dict.dat");
int8_t f_code = 0;
for (size_t i = 0; i+1 < g_off.size(); ++i)
    if (std::string_view(g_dat.data()+g_off[i], g_off[i+1]-g_off[i]) == "f") { f_code = (int8_t)(i+1); break; }
auto n_gender = read_vec<int8_t>(store + "/name/gender.bin");
```

### name.name (varlen) — LIKE `%Ang%`
Files: `name/name.off`, `name/name.dat`. Scan once.

### cast_info.note (varlen), role_id, movie_id, person_id, person_role_id
Files: `cast_info/note.{off,dat}`, `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin`. Length 36244344. `person_role_id` is nullable (`INT32_MIN`).
```cpp
auto ci_note_off = read_vec<int64_t>(store + "/cast_info/note.off");
std::string ci_note_dat = read_file(store + "/cast_info/note.dat");
absl::flat_hash_set<std::string_view> ci_set = {"(voice)","(voice: Japanese version)","(voice) (uncredited)","(voice: English version)"};
auto note_in = [&](int32_t r){
    int64_t a=ci_note_off[r], b=ci_note_off[r+1];
    return ci_set.contains(std::string_view(ci_note_dat.data()+a, b-a));
};
```

### movie_companies.note, movie_id, company_id
Files: `movie_companies/note.{off,dat}`, `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. Length 2609129. `mc.note IS NOT NULL` → row slice non-empty. LIKE `%(USA)%` OR `%(worldwide)%` via memmem.

### title.title, title.production_year
Files: `title/title.{off,dat}`, `title/production_year.bin` (int32 length 2528312). `BETWEEN 2005 AND 2015`.

### aka_name.person_id, aka_name.name
Files: `aka_name/person_id.bin`, `aka_name/name.{off,dat}`. Length 901343 sorted by person_id.

### char_name.id, char_name.name
Files: `char_name/id.bin` (identity length 3140339), `char_name/name.{off,dat}`. Dense-PK: name for chn.id=v at row v-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK |
| name | 4,167,491 | id | dense-PK; gender int8 dict |
| char_name | 3,140,339 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on person_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Aggregation: MIN(an.name), MIN(chn.name), MIN(t.title).

Driver: build candidate `n.id` set: scan name (`gender==f_code` AND `name` LIKE `%Ang%`). f restricts to ~1/2 of names; Ang substring is moderate → maybe ~few k.

Approach (person-driven):
1. Resolve `rt_id_actress`, `us_code`, `f_code`. Build candidate `pids` from name scan.
2. For each `pid`:
   - `aka_name__person_id` range — if empty skip (an.person_id=n.id).
   - `cast_info__person_id` CSR range → for each ci row r:
     - filter `role_id==rt_id_actress`, note IN set, `person_role_id != INT32_MIN`.
     - `mv=ci.movie_id[r]`. Check `title.production_year[mv-1] BETWEEN 2005 AND 2015`.
     - `movie_companies__movie_id[mv..mv+1]`: any mc with `cn.country_code[mc.company_id-1]==us_code` AND mc.note non-empty AND (`%(USA)%` OR `%(worldwide)%`).
   - For surviving (pid, ci_row, mv): chn_id = ci.person_role_id[r]. Read `char_name.name` at row `chn_id-1`.
   - Update three MINs.

Selectivities:
- `n.gender='f'` ≈ 1/3 of name rows. `LIKE '%Ang%'` further trims to ~k.
- `rt.role='actress'` → 1 role.
- `ci.note IN (4 voice strings)` → small fraction.
- Year 2005-2015 → ~half of titles.
- `cn.country_code='[us]'` → most companies, but combined with mc.note LIKE → small.

LIKE notes: `%Ang%`, `%(USA)%`, `%(worldwide)%` all substring; use `memmem`. IN set uses `flat_hash_set`.

## Indexes

### cast_info__person_id (CSR)
Files: `_idx/cast_info__person_id__offsets.bin` (int32, 4167493), `_idx/cast_info__person_id__rowids.bin` (int32, 36244344).
```cpp
auto cipid_off = read_vec<int32_t>(store + "/_idx/cast_info__person_id__offsets.bin");
auto cipid_row = read_vec<int32_t>(store + "/_idx/cast_info__person_id__rowids.bin");
int32_t lo = cipid_off[pid], hi = cipid_off[pid+1];
for (int32_t k = lo; k < hi; ++k) { int32_t r = cipid_row[k]; /* ci row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).

### char_name lookup
No index needed — `chn.id = ci.person_role_id[r]` → read `char_name.name` directly at row `chn_id - 1` (dense PK).
