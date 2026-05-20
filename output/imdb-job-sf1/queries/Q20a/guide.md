## SQL

```sql
SELECT MIN(t.title) AS complete_downey_ironman_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, keyword AS k, kind_type AS kt,
     movie_keyword AS mk, name AS n, title AS t
WHERE cct1.kind='cast'
  AND cct2.kind LIKE '%complete%'
  AND chn.name NOT LIKE '%Sherlock%'
  AND (chn.name LIKE '%Tony%Stark%' OR chn.name LIKE '%Iron%Man%')
  AND k.keyword IN ('superhero','sequel','second-part','marvel-comics','based-on-comic','tv-special','fight','violence')
  AND kt.kind='movie'
  AND t.production_year>1950
  AND kt.id=t.kind_id AND t.id=mk.movie_id AND t.id=ci.movie_id AND t.id=cc.movie_id
  AND chn.id=ci.person_role_id AND n.id=ci.person_id
  AND k.id=mk.keyword_id
  AND cct1.id=cc.subject_id AND cct2.id=cc.status_id;
```

## Column Reference

### comp_cast_type.kind (varlen, 4 rows)
Files: `comp_cast_type/kind.off` (5), `comp_cast_type/kind.dat`. Scan once.
```cpp
auto cct_off = read_vec<int64_t>(store + "/comp_cast_type/kind.off");
std::string cct_dat = read_file(store + "/comp_cast_type/kind.dat");
int32_t cct1_id = 0;            // 'cast'
std::vector<int32_t> cct2_ids;  // kind LIKE '%complete%'
for (size_t i=0; i+1<cct_off.size(); ++i) {
    std::string_view s(cct_dat.data()+cct_off[i], cct_off[i+1]-cct_off[i]);
    if (s=="cast") cct1_id=(int32_t)(i+1);
    if (memmem(s.data(), s.size(), "complete", 8)) cct2_ids.push_back((int32_t)(i+1));
}
// cct2_ids likely = {3,4} for 'complete', 'complete+verified'.
```

### kind_type.kind (varlen, 7 rows)
Files: `kind_type/kind.off`, `kind_type/kind.dat`. Resolve `'movie'` → `kt_movie`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off` (134171), `keyword/keyword.dat`. Resolve 8 IN-list literals → set `K_ids`.

### char_name.name (varlen)
Files: `char_name/name.off`, `char_name/name.dat`. Length 3140339. LIKE filters:
`name NOT LIKE '%Sherlock%' AND (name LIKE '%Tony%Stark%' OR name LIKE '%Iron%Man%')`.
Two-pointer matching via `memmem`. char_name.id dense — row `chn_id-1`.

### complete_cast.movie_id, subject_id, status_id (int32)
Files: `complete_cast/movie_id.bin` (nullable), `.../subject_id.bin`, `.../status_id.bin`. 135086 rows; sorted by movie_id.

### cast_info.movie_id, person_id, person_role_id (int32)
Files in `cast_info/`. 36244344 rows; sorted by movie_id.

### movie_keyword.movie_id, keyword_id (int32)
Files: `movie_keyword/movie_id.bin`, `.../keyword_id.bin`. 4523930 rows; sorted by movie_id.

### name.id, name (int32/varlen)
n is dense; no name filter in Q20a. ci.person_id used only to confirm ref.

### title.id, title, production_year, kind_id (int32/varlen/int32/int32)
Files in `title/`. `production_year > 1950 AND != INT32_MIN`; `kind_id == kt_movie`.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| comp_cast_type | 4 | id | dense-PK |
| kind_type | 7 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| complete_cast | 135,086 | movie_id | offsets_only movie_id; CSR subject_id/status_id |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_role_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only movie_id; CSR keyword_id |
| title | 2,528,312 | id | dense-PK; CSR kind_id |

## Query Analysis

Join graph:
```
kt --kind_id-- t --movie_id-- mk --keyword_id-- k
                |--movie_id-- ci --person_role_id-- chn
                |              \--person_id-- n
                +--movie_id-- cc --subject_id-- cct1
                               \--status_id-- cct2
```

Aggregation: MIN(t.title).

Driver: candidate movies from movie_keyword joined to k IN-list (~8 keyword ids) — smallest join. Combine with year>1950 and kind=='movie' and complete_cast existence.

Plan:
1. Resolve `cct1_id` (single id for 'cast') and `cct2_ids` (set, likely {complete, complete+verified}).
2. Resolve `kt_movie`.
3. Resolve `K_ids` (8 keyword ids; some may be NotFound→0).
4. Build candidate char_name set `CHN`: scan char_name.name; keep ids where name passes the Tony/Iron AND NOT Sherlock pattern. Likely small.
5. For each `kid` in `K_ids`: walk `movie_keyword__keyword_id` CSR → set of mv ids. Union into `M`.
6. For each `mv` in M:
   - Check `title.kind_id[mv-1]==kt_movie` and `production_year[mv-1] > 1950 AND != INT32_MIN`.
   - Probe `complete_cast__movie_id` range. For each cc row check `subject_id==cct1_id` AND `status_id ∈ cct2_ids`. If found → pass.
   - Probe `cast_info__movie_id` range. For each ci row: `person_role_id ∈ CHN`. (chn.id is dense — store `CHN` as `vector<bool>` of size 3140339+1.) If any hit → update MIN(title[mv-1]).

Alternative: since cct1/cct2 only have 4 rows total, you can scan `complete_cast` (135K) once and test `subject_id == cct1_id` AND `status_id ∈ cct2_ids` directly without using the CSR — cheaper than two CSR walks. Collect set of `mv` candidates from cc this way.

Selectivities:
- complete_cast rows passing cct1/cct2 → small fraction of 135K.
- 8-keyword IN-list → tens to hundreds of K mk rows.
- char_name LIKE Tony/Iron → small.
- year > 1950 → broad.

LIKE notes:
- `%Sherlock%`: NOT — exclude.
- `%Tony%Stark%`: two-pointer (Tony then Stark).
- `%Iron%Man%`: two-pointer (Iron then Man).
- `%complete%`: scan 4-row comp_cast_type at startup.

## Indexes

### movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930).
```cpp
auto mkk_off = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
auto mkk_row = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
int32_t lo=mkk_off[kid], hi=mkk_off[kid+1];
for (int32_t k2=lo; k2<hi; ++k2) { int32_t r=mkk_row[k2]; int32_t mv=mk_mid[r]; }
```

### complete_cast__movie_id (offsets_only, primary)
File: `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314). cc sorted by movie_id.

### complete_cast__subject_id (CSR) / complete_cast__status_id (CSR)
Files: `_idx/complete_cast__subject_id__{offsets,rowids}.bin`, `_idx/complete_cast__status_id__{offsets,rowids}.bin`. parent_max=4 (tiny dimensions). Useful if driving from cct1/cct2; given table is only 135K rows, a direct scan is often equivalent.

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin`.

### title__kind_id (CSR)
Files: `_idx/title__kind_id__{offsets,rowids}.bin`. parent_max=7. Could enumerate all movies with `kind_id==kt_movie`, but per-movie lookup is fine.

### cast_info__person_role_id (CSR) — alt
Files: `_idx/cast_info__person_role_id__{offsets,rowids}.bin`. Use to drive from `CHN` set directly into ci rows.
