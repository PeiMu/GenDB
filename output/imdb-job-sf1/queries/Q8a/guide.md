## SQL

```sql
SELECT MIN(an1.name) AS actress_pseudonym,
       MIN(t.title) AS japanese_movie_dubbed
FROM aka_name AS an1, cast_info AS ci, company_name AS cn,
     movie_companies AS mc, name AS n1, role_type AS rt, title AS t
WHERE ci.note ='(voice: English version)'
  AND cn.country_code ='[jp]'
  AND mc.note LIKE '%(Japan)%'  AND mc.note NOT LIKE '%(USA)%'
  AND n1.name LIKE '%Yo%'       AND n1.name NOT LIKE '%Yu%'
  AND rt.role ='actress'
  AND an1.person_id = n1.id     AND n1.id = ci.person_id
  AND ci.movie_id = t.id        AND t.id = mc.movie_id
  AND mc.company_id = cn.id     AND ci.role_id = rt.id
  AND an1.person_id = ci.person_id AND ci.movie_id = mc.movie_id;
```

## Column Reference

### role_type.role (varlen)
Files: `role_type/role.off` (int64, 13), `role_type/role.dat`.
```cpp
auto off = read_vec<int64_t>(store + "/role_type/role.off");
std::string dat = read_file(store + "/role_type/role.dat");
int32_t rt_id = 0;
for (size_t i = 0; i+1 < off.size(); ++i)
    if (std::string_view(dat.data()+off[i], off[i+1]-off[i]) == "actress") { rt_id = (int32_t)(i+1); break; }
```

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t jp_code = 0;
for (size_t i = 0; i+1 < cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[jp]") { jp_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");  // length 234997
```

### cast_info.note (varlen)
Files: `cast_info/note.off`, `cast_info/note.dat`. Equality compare on per-row slice.
```cpp
auto ci_note_off = read_vec<int64_t>(store + "/cast_info/note.off");
std::string ci_note_dat = read_file(store + "/cast_info/note.dat");
std::string_view literal = "(voice: English version)";
auto note_eq = [&](int32_t r){
    int64_t a=ci_note_off[r], b=ci_note_off[r+1];
    return (b-a)==(int64_t)literal.size() && memcmp(ci_note_dat.data()+a, literal.data(), literal.size())==0;
};
```

### cast_info.role_id, cast_info.movie_id, cast_info.person_id (int32)
Files: `cast_info/role_id.bin`, `cast_info/movie_id.bin`, `cast_info/person_id.bin`. Plain int32 length 36244344. Read with mmap.

### movie_companies.note (varlen)
Files: `movie_companies/note.off`, `movie_companies/note.dat`. Apply LIKE `%(Japan)%` and NOT LIKE `%(USA)%` via `memmem`.

### movie_companies.company_id, movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin`. Length 2609129.

### name.name (varlen)
Files: `name/name.off`, `name/name.dat`. LIKE `%Yo%` AND NOT LIKE `%Yu%` via `memmem` on row slice; length 4167491.

### aka_name.person_id, aka_name.name (int32 / varlen)
Files: `aka_name/person_id.bin`, `aka_name/name.off`, `aka_name/name.dat`. aka_name is sorted by person_id; use `aka_name__person_id` to fetch ranges per n.id.

### title.id, title.title (int32 / varlen)
`title/id.bin` is identity; `title/title.off`, `title/title.dat`. Dense-PK: title for id=v at row v-1.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| role_type | 12 | id | dense-PK |
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| name | 4,167,491 | id | dense-PK |
| aka_name | 901,343 | person_id | offsets_only on person_id |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on person_id/role_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id; CSR on company_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph (star around title and cast_info):
```
rt --role_id-- ci --movie_id-- t --movie_id-- mc --company_id-- cn
                |                                  
                +--person_id-- n --person_id-- an1
```

Aggregation: MIN(an1.name), MIN(t.title). MIN over varlen → keep running smallest string.

Driver: build candidate `n.id` set by scanning `name/name.{off,dat}` for `%Yo%` AND NOT `%Yu%`. Resolve `rt_id_actress` (single id). Resolve `cn_jp_code` (single int16 dict code).

Approach A (person-driven; small `name` filter expected):
1. Scan `name` for LIKE pattern → ~candidate person ids. n.gender is unused here.
2. For each candidate `pid`: walk `cast_info__person_id` (CSR) → ci rows; filter `ci.role_id==rt_id_actress` and `ci.note=='(voice: English version)'`.
3. For surviving ci row, take `mv=ci.movie_id[r]`. For movie, walk `movie_companies__movie_id` offsets-only range; for each mc row apply LIKE filters and check `cn.country_code[mc.company_id-1]==jp_code`.
4. Confirm `an1` exists via `aka_name__person_id` offsets-only on `pid` (range non-empty); read `aka_name.name` from that range for MIN.
5. MIN(t.title) at title row `mv-1`.

Selectivities (rough):
- `rt.role='actress'` → 1 of 12 role_types.
- `ci.note='(voice: English version)'` → small fraction of 36M cast_info.
- `cn.country_code='[jp]'` → small fraction of 235K company_name rows.
- `n.name LIKE '%Yo%' AND NOT '%Yu%'` → tens of thousands of names.
- Combined → tiny result set; MIN merges easily.

LIKE notes: all wildcards are `%X%` → use `memmem(dat+off[r], len, "X", k)`; prune by length first.

## Indexes

### cast_info__person_id (CSR)
Files: `_idx/cast_info__person_id__offsets.bin` (int32, 4167493), `_idx/cast_info__person_id__rowids.bin` (int32, 36244344). Sentinel slot 0 = NULL/<1 count.
```cpp
auto cipid_off = read_vec<int32_t>(store + "/_idx/cast_info__person_id__offsets.bin");
auto cipid_row = read_vec<int32_t>(store + "/_idx/cast_info__person_id__rowids.bin");
int32_t lo = cipid_off[pid], hi = cipid_off[pid+1];
for (int32_t k = lo; k < hi; ++k) { int32_t r = cipid_row[k]; /* ci row r */ }
```

### aka_name__person_id (offsets_only)
File: `_idx/aka_name__person_id__offsets.bin` (int32, 4167493). aka_name is sorted by person_id.
```cpp
auto akpid_off = read_vec<int32_t>(store + "/_idx/aka_name__person_id__offsets.bin");
int32_t alo = akpid_off[pid], ahi = akpid_off[pid+1];
for (int32_t r = alo; r < ahi; ++r) { /* aka_name row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mcmid_off = read_vec<int32_t>(store + "/_idx/movie_companies__movie_id__offsets.bin");
int32_t mlo = mcmid_off[mv], mhi = mcmid_off[mv+1];
for (int32_t r = mlo; r < mhi; ++r) { /* mc row r */ }
```
