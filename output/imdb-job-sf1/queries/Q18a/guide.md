## SQL

```sql
SELECT MIN(mi.info) AS movie_budget,
       MIN(mi_idx.info) AS movie_votes,
       MIN(t.title) AS movie_title
FROM cast_info AS ci, info_type AS it1, info_type AS it2,
     movie_info AS mi, movie_info_idx AS mi_idx,
     name AS n, title AS t
WHERE ci.note IN ('(producer)','(executive producer)')
  AND it1.info = 'budget'
  AND it2.info = 'votes'
  AND n.gender = 'm'
  AND n.name LIKE '%Tim%'
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id
  AND t.id = ci.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off` (int64, 114), `info_type/info.dat`. Dense-PK; scan once and resolve both `'budget'` → `it1_id` and `'votes'` → `it2_id`.
```cpp
auto it_off = read_vec<int64_t>(store + "/info_type/info.off");
std::string it_dat = read_file(store + "/info_type/info.dat");
int32_t it_budget=0, it_votes=0;
for (size_t i=0; i+1<it_off.size(); ++i) {
    std::string_view s(it_dat.data()+it_off[i], it_off[i+1]-it_off[i]);
    if (s=="budget") it_budget=(int32_t)(i+1);
    else if (s=="votes") it_votes=(int32_t)(i+1);
}
```

### name.gender (int8 dict)
Files: `name/gender.bin` (int8, 4167491), `name/gender.dict.off`, `name/gender.dict.dat`.
```cpp
auto g_off = read_vec<int64_t>(store + "/name/gender.dict.off");
std::string g_dat = read_file(store + "/name/gender.dict.dat");
int8_t g_m = 0;
for (size_t i=0; i+1<g_off.size(); ++i)
    if (std::string_view(g_dat.data()+g_off[i], g_off[i+1]-g_off[i])=="m") { g_m=(int8_t)(i+1); break; }
auto n_gender = read_vec<int8_t>(store + "/name/gender.bin");
```

### name.name (varlen)
Files: `name/name.off`, `name/name.dat`. LIKE `%Tim%` via `memmem` on row slice.

### cast_info.note (varlen)
Files: `cast_info/note.off`, `cast_info/note.dat`. IN list — match exact length+memcmp for `(producer)` and `(executive producer)`.

### cast_info.person_id, movie_id (int32)
Files: `cast_info/person_id.bin`, `cast_info/movie_id.bin`. Length 36244344. ci is sorted by movie_id.

### movie_info.info_type_id, movie_id, info (int32/int32/varlen)
Files: `movie_info/info_type_id.bin`, `movie_info/movie_id.bin`, `movie_info/info.off`, `movie_info/info.dat`. Length 14835720; sorted by movie_id.

### movie_info_idx.info_type_id, movie_id, info (int32/int32/varlen)
Files: `movie_info_idx/info_type_id.bin`, `.../movie_id.bin`, `.../info.off`, `.../info.dat`. Length 1380035; sorted by movie_id.

### title.id, title.title (int32/varlen)
`title/id.bin` is identity (2528312); `title/title.off`, `title/title.dat`. Dense-PK; row v-1 for id v.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| name | 4,167,491 | id | dense-PK; gender int8 dict |
| cast_info | 36,244,344 | movie_id | offsets_only on movie_id; CSR on person_id |
| movie_info | 14,835,720 | movie_id | offsets_only on movie_id; CSR on info_type_id |
| movie_info_idx | 1,380,035 | movie_id | offsets_only on movie_id; CSR on info_type_id |
| title | 2,528,312 | id | dense-PK |

## Query Analysis

Join graph (star on t):
```
n --person_id-- ci --movie_id-- t --movie_id-- mi   --info_type_id-- it1
                                  --movie_id-- mi_idx --info_type_id-- it2
```

Aggregations: MIN over three varlens — track running smallest.

Driver: build candidate `n.id` set by scanning `name/name.{off,dat}` for `%Tim%` AND `n.gender==g_m`. Likely tens of thousands.

Plan:
1. Resolve `it_budget`, `it_votes`, `g_m`.
2. Build set S of pids satisfying gender filter and name LIKE.
3. For each pid in S, walk `cast_info__person_id` CSR; for each ci row test `note IN {(producer),(executive producer)}`; collect `mv=movie_id[r]`.
4. For each mv: scan mi range via `movie_info__movie_id` offsets; keep rows with `info_type_id==it_budget`. Scan mi_idx range via `movie_info_idx__movie_id`; keep rows with `info_type_id==it_votes`. Both must be non-empty.
5. Update MIN(mi.info), MIN(mi_idx.info), MIN(title[mv-1]).

Selectivities:
- `n.gender='m' AND name LIKE '%Tim%'` → ~tens of thousands of pids.
- `ci.note IN (...)` → small fraction of 36M.
- `it.info='budget'` → 1 id; mi rows for that type ~50K. Same scale for votes in mi_idx.
- Path heavily reduced; mi/mi_idx range probes are tight.

LIKE notes: `%Tim%` → `memmem` after length prune (>=3 bytes).

## Indexes

### cast_info__person_id (CSR)
Files: `_idx/cast_info__person_id__offsets.bin` (int32, 4167493), `_idx/cast_info__person_id__rowids.bin` (int32, 36244344). Slot 0 = NULL/<1 count.
```cpp
auto cipid_off = read_vec<int32_t>(store + "/_idx/cast_info__person_id__offsets.bin");
auto cipid_row = read_vec<int32_t>(store + "/_idx/cast_info__person_id__rowids.bin");
int32_t lo=cipid_off[pid], hi=cipid_off[pid+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r=cipid_row[k]; /* ci row r */ }
```

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mim_off = read_vec<int32_t>(store + "/_idx/movie_info__movie_id__offsets.bin");
int32_t lo=mim_off[mv], hi=mim_off[mv+1];
for (int32_t r=lo; r<hi; ++r) if (mi_itid[r]==it_budget) { /* row r */ }
```

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mixm_off = read_vec<int32_t>(store + "/_idx/movie_info_idx__movie_id__offsets.bin");
int32_t lo=mixm_off[mv], hi=mixm_off[mv+1];
for (int32_t r=lo; r<hi; ++r) if (mix_itid[r]==it_votes) { /* row r */ }
```

Note: `movie_info__info_type_id` and `movie_info_idx__info_type_id` CSRs exist but are not needed here — driving from movie ranges is cheaper than from info_type (only 1 id).
