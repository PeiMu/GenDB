## SQL

```sql
SELECT MIN(mi.info) AS movie_budget,
       MIN(mi_idx.info) AS movie_votes,
       MIN(t.title) AS movie_title
FROM cast_info AS ci, info_type AS it1, info_type AS it2,
     movie_info AS mi, movie_info_idx AS mi_idx,
     name AS n, title AS t
WHERE ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND it1.info = 'genres'
  AND it2.info = 'rating'
  AND mi.info IN ('Horror','Thriller')
  AND mi.note IS NULL
  AND mi_idx.info > '8.0'
  AND n.gender IS NOT NULL AND n.gender = 'f'
  AND t.production_year BETWEEN 2008 AND 2014
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id AND t.id = ci.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. Scan once, resolve `'genres'` → `it_genres`, `'rating'` → `it_rating`.

### name.gender (int8 dict)
Files: `name/gender.bin`, `name/gender.dict.off`, `name/gender.dict.dat`. Resolve `'f'` → `g_f` (non-zero code).
```cpp
auto g_off = read_vec<int64_t>(store + "/name/gender.dict.off");
std::string g_dat = read_file(store + "/name/gender.dict.dat");
int8_t g_f = 0;
for (size_t i=0; i+1<g_off.size(); ++i)
    if (std::string_view(g_dat.data()+g_off[i], g_off[i+1]-g_off[i])=="f") { g_f=(int8_t)(i+1); break; }
auto n_gender = read_vec<int8_t>(store + "/name/gender.bin");
// n.gender IS NOT NULL AND = 'f'  =>  n_gender[i] == g_f  (g_f != 0)
```

### cast_info.note (varlen)
Files: `cast_info/note.off`, `cast_info/note.dat`. IN-list of 5 strings; build `flat_hash_set<string_view>`.

### cast_info.person_id, movie_id (int32)
Files: `cast_info/person_id.bin`, `cast_info/movie_id.bin`. Length 36244344; sorted by movie_id.

### movie_info.info_type_id, movie_id, info, note (int32/int32/varlen/varlen)
Files: `movie_info/info_type_id.bin`, `.../movie_id.bin`, `.../info.off+.dat`, `.../note.off+.dat`. Length 14835720. `mi.note IS NULL` ⇔ `note.off[i]==note.off[i+1]`.

### movie_info_idx.info_type_id, movie_id, info (int32/int32/varlen)
Files: `movie_info_idx/info_type_id.bin`, `.../movie_id.bin`, `.../info.off+.dat`. Length 1380035. `info > '8.0'` is bytewise string compare on row slice.

### title.id, title, production_year (int32/varlen/int32)
Files: `title/id.bin` (identity), `title/title.off+.dat`, `title/production_year.bin`. Length 2528312. NULL = INT32_MIN; require `py != INT32_MIN AND 2008<=py<=2014`.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_id |
| movie_info | 14,835,720 | movie_id | offsets_only movie_id |
| movie_info_idx | 1,380,035 | movie_id | offsets_only movie_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph:
```
n --person_id-- ci --movie_id-- t --movie_id-- mi      --info_type_id-- it1
                                  --movie_id-- mi_idx  --info_type_id-- it2
```

Driver: scan `title/production_year.bin` once, retain title ids with 2008..2014 → tight movie set (~few hundred K rows). This is far smaller than name/ci scans.

Plan:
1. Resolve `it_genres`, `it_rating`, `g_f`.
2. Scan title.production_year (length 2528312) collecting movie ids T with year in [2008,2014].
3. For each `mv` in T:
   - Walk `movie_info__movie_id` range; require row with `info_type_id==it_genres`, `info IN {Horror,Thriller}`, `note` empty. If none, skip.
   - Walk `movie_info_idx__movie_id` range; require row with `info_type_id==it_rating` and `info > "8.0"` (lex compare). If none, skip.
   - Walk `cast_info__movie_id` range; for each ci row test `note` IN-list; check `n_gender[person_id-1] == g_f`. If any, update MINs (mi.info, mi_idx.info, title[mv-1]).

Selectivities:
- production_year BETWEEN 2008..2014 → ~12% of titles.
- it_genres + mi.info IN {Horror,Thriller} + note NULL → small.
- mi_idx info > '8.0' → small (~10% of rating rows).
- ci.note IN (5 writer roles) → very small.
- Combined → tiny.

String compare `> '8.0'`: lex bytewise (`memcmp(a,b,min(la,lb))`, then length tiebreak).

LIKE notes: none here; only equality / IN / range / NULL.

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto cim_off = read_vec<int32_t>(store + "/_idx/cast_info__movie_id__offsets.bin");
int32_t lo=cim_off[mv], hi=cim_off[mv+1];
for (int32_t r=lo; r<hi; ++r) { /* ci row r */ }
```

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).

Note: CSRs `movie_info__info_type_id` (113 keys) and `movie_info_idx__info_type_id` exist but driving from the year-filtered title set is more selective.
