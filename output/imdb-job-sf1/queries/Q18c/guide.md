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
  AND it2.info = 'votes'
  AND mi.info IN ('Horror','Action','Sci-Fi','Thriller','Crime','War')
  AND n.gender = 'm'
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id AND t.id = ci.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. Resolve `'genres'` → `it_genres`, `'votes'` → `it_votes` in one 113-row scan.

### name.gender (int8 dict)
Files: `name/gender.bin` (4167491), `.dict.off`, `.dict.dat`. Resolve `'m'` → `g_m` non-zero.
```cpp
auto n_gender = read_vec<int8_t>(store + "/name/gender.bin");
// keep n_gender[i] == g_m
```

### cast_info.note, person_id, movie_id
Files: `cast_info/note.off+.dat`, `cast_info/person_id.bin`, `cast_info/movie_id.bin`. 36244344 rows; ci sorted by movie_id. IN-list of 5 writer notes — build hash set on `string_view`.

### movie_info.info, info_type_id, movie_id (varlen/int32/int32)
Files: `movie_info/info.off+.dat`, `.../info_type_id.bin`, `.../movie_id.bin`. 14835720 rows. `info IN {6 genres}` → length-prune then equality check.

### movie_info_idx.info, info_type_id, movie_id
Files: `movie_info_idx/info.off+.dat`, `.../info_type_id.bin`, `.../movie_id.bin`. 1380035 rows.

### title.id, title (int32/varlen)
Files: `title/id.bin` identity, `title/title.off+.dat`. Length 2528312. No year filter here.

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

Join graph (same star as Q18a/b):
```
n --person_id-- ci --movie_id-- t --movie_id-- mi      --info_type_id-- it1
                                  --movie_id-- mi_idx  --info_type_id-- it2
```

No year filter, no name LIKE. Best driver is `mi.info IN (...)` joined with `info_type_id==it_genres`.

Plan:
1. Resolve `it_genres`, `it_votes`, `g_m`.
2. Build `string_view` hash set of 6 genre names. Scan movie_info linearly; for each row r: if `info_type_id[r]==it_genres` and `info[r] in set` → collect `mv=movie_id[r]` into set T. (Also acceptable: use `movie_info__info_type_id` CSR for it_genres to limit to ~genres rows; both scans are O(rows).)
3. For each `mv` in T:
   - Probe `movie_info_idx__movie_id` range; require row with `info_type_id==it_votes`. Capture its `info` for MIN.
   - Probe `cast_info__movie_id` range; for each ci row test `note IN writer-set` AND `n_gender[person_id-1] == g_m`. If any hit, update MINs.

Selectivities:
- it_genres mi rows ≈ several million genre tags; intersected with IN-set of 6 → still hundreds of K.
- mi_idx votes per movie sparse but common.
- ci.note IN (writer notes) → small.
- n.gender='m' → ~majority of names.

LIKE notes: none here. `mi.note` is unconstrained (no IS NULL in this variant).

## Indexes

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin` (int32, 2528314).

### movie_info__info_type_id (CSR)
Files: `_idx/movie_info__info_type_id__offsets.bin` (int32, 115), `_idx/movie_info__info_type_id__rowids.bin` (int32, 14835720). Use when iterating mi rows with `info_type_id==it_genres`.
```cpp
auto mit_off = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__offsets.bin");
auto mit_row = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__rowids.bin");
int32_t lo=mit_off[it_genres], hi=mit_off[it_genres+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r=mit_row[k]; /* mi row r */ }
```

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314). Alternative when driving by movie set.
