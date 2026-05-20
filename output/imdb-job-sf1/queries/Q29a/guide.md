# Q29a Guide

## SQL
```sql
SELECT MIN(chn.name) AS voiced_char,
       MIN(n.name) AS voicing_actress,
       MIN(t.title) AS voiced_animation
FROM aka_name AS an, complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, company_name AS cn, info_type AS it,
     info_type AS it3, keyword AS k, movie_companies AS mc, movie_info AS mi,
     movie_keyword AS mk, name AS n, person_info AS pi, role_type AS rt, title AS t
WHERE cct1.kind = 'cast' AND cct2.kind = 'complete+verified'
  AND chn.name = 'Queen'
  AND ci.note IN ('(voice)','(voice) (uncredited)','(voice: English version)')
  AND cn.country_code = '[us]'
  AND it.info = 'release dates' AND it3.info = 'trivia'
  AND k.keyword = 'computer-animation'
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%200%' OR mi.info LIKE 'USA:%200%')
  AND n.gender = 'f' AND n.name LIKE '%An%'
  AND rt.role = 'actress'
  AND t.title = 'Shrek 2'
  AND t.production_year BETWEEN 2000 AND 2010
  AND t.id = mi.movie_id AND t.id = mc.movie_id AND t.id = ci.movie_id
  AND t.id = mk.movie_id AND t.id = cc.movie_id
  AND cn.id = mc.company_id AND it.id = mi.info_type_id
  AND n.id = ci.person_id AND rt.id = ci.role_id
  AND n.id = an.person_id AND chn.id = ci.person_role_id
  AND n.id = pi.person_id AND it3.id = pi.info_type_id
  AND k.id = mk.keyword_id
  AND cct1.id = cc.subject_id AND cct2.id = cc.status_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Resolve `cast_id` (==`'cast'`) and `verified_id` (==`'complete+verified'`).

### char_name.name (filter, varlen) — `chn.name = 'Queen'` EXACT EQUALITY
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK → id = row+1.
- Use: scan once collecting EVERY row i where `[off[i],off[i+1])` equals exactly `"Queen"`. There may be multiple. Push (i+1) into `chn_set`. (Length prefilter: skip rows whose length != 5.)

### cast_info.movie_id / .person_id / .person_role_id / .role_id / .note
- Files: `cast_info/movie_id.bin`, `cast_info/person_id.bin`, `cast_info/person_role_id.bin` (nullable), `cast_info/role_id.bin`, `cast_info/note.off`+`.dat`. Rows: 36244344; sorted by movie_id.
- Use: t_id is fixed to 1 (or few) — range probe via `cast_info__movie_id`. Filter: `person_role_id ∈ chn_set`, `role_id == actress_id`, `note ∈ {3 literals}`.

### company_name.name / .country_code
- Files: `company_name/name.off`+`.dat`; `company_name/country_code.bin`+`.dict.off`+`.dict.dat`; rows: 234997; dense PK.
- Resolve `us_code`; `cn_us_set = {ids where code == us_code}`.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Resolve `it_id` (==`'release dates'`) and `it3_id` (==`'trivia'`). NOTE: `it.id == mi.info_type_id` (for movie_info), `it3.id == pi.info_type_id` (for person_info).

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`. Resolve `cam_kw_id` (==`'computer-animation'`).

### movie_companies.movie_id / .company_id
- Files: `movie_companies/movie_id.bin`, `movie_companies/company_id.bin`. Filter: `company_id ∈ cn_us_set`.

### movie_info.movie_id / .info_type_id / .info
- Files: `movie_info/movie_id.bin`, `movie_info/info_type_id.bin`, `movie_info/info.off`+`.dat`.
- Filter: `info_type_id == it_id && (info STARTS WITH 'Japan:' OR info STARTS WITH 'USA:') AND info CONTAINS '200'`. Equivalent to `LIKE 'Japan:%200%'` or `LIKE 'USA:%200%'`.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.
- Filter: `keyword_id == cam_kw_id`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Filter: `subject_id == cast_id && status_id == verified_id`.

### name.name / .gender (filter+project)
- Files: `name/name.off`+`.dat`; `name/gender.bin`+`.dict.off`+`.dict.dat`; rows: 4167491; dense PK.
- Resolve `f_code` (==`'f'`) from gender dict. Build `n_set` of ids where `gender_code == f_code` AND `name` contains `An` (memmem). This is large (~hundreds of thousands); keep as a sorted vector or roaring bitmap. **Alternative**: since we drive from a single movie `Shrek 2`, we may instead lazily test the predicate per ci.person_id (avoid pre-materializing the full set).

### aka_name.person_id (existence join, int32_t)
- File: `aka_name/person_id.bin`; rows: 901343; sorted by person_id.
- Use: existence check `an.person_id == ci.person_id` via offsets_only `aka_name__person_id` index. Probe `lo!=hi`.

### person_info.person_id / .info_type_id (existence join + filter)
- Files: `person_info/person_id.bin`, `person_info/info_type_id.bin`; rows: 2963664; sorted by person_id.
- Critical: person_info has TWO indexes — primary `person_info__person_id` (offsets_only) and secondary `person_info__info_type_id` (CSR).
- Q29 strategy: ci yields a small set of candidate persons (Queens voicing actresses in Shrek 2). For each `ci.person_id`, use `person_info__person_id` to get the per-person row range, then test `info_type_id == it3_id`. Do NOT scan all 2.9M pi rows via the it3 CSR — the per-person range is tiny.

### role_type.role (filter, varlen)
- Files: `role_type/role.off`, `role_type/role.dat`; rows: 12; dense PK.
- Resolve `actress_id` (==`'actress'`).

### title.id / .title / .production_year — DRIVER (`t.title = 'Shrek 2'` EXACT)
- Files: `title/id.bin`, `title/title.off`+`.dat`, `title/production_year.bin`; rows: 2528312; dense PK.
- Use: scan title.off+title.dat ONCE for length==7 entries equal to `"Shrek 2"`. Collect 1+ row indices (likely 1 main row plus possibly remakes). For each, `t_id = row+1`; require `production_year BETWEEN 2000 AND 2010`.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| role_type | 12 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| char_name | 3140339 | dim (filter set) | id | 100000 |
| company_name | 234997 | dim (filter) | id | 100000 |
| name | 4167491 | dim (filter+project) | id | 100000 |
| aka_name | 901343 | existence | person_id | 100000 |
| title | 2528312 | very narrow driver | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |
| cast_info | 36244344 | fact | movie_id | 200000 |
| person_info | 2963664 | per-person probe | person_id | 100000 |

## Query Analysis
- The two strongest filters by far: `t.title='Shrek 2'` and `chn.name='Queen'`. Resolve both up front by scanning the title and char_name varlens.
- Driving plan:
  1. Scan `title/title.off+.dat` → list of t_id candidates (expect very few, often 1).
  2. For each t_id, check `production_year ∈ [2000,2010]` and `kind_id` (no kind predicate here — skip).
  3. Probe `movie_keyword__movie_id` → require `keyword_id == cam_kw_id`.
  4. Probe `complete_cast__movie_id` → require subject==cast_id & status==verified_id.
  5. Probe `movie_companies__movie_id` → require `company_id ∈ cn_us_set`.
  6. Probe `movie_info__movie_id` → require `info_type_id==it_id` AND info `LIKE 'Japan:%200%' OR 'USA:%200%'`.
  7. Probe `cast_info__movie_id` → require `person_role_id ∈ chn_set`, `role_id==actress_id`, `note ∈ 3-literal set`. For each surviving ci row: yield `ci.person_id`.
  8. For each candidate person p = ci.person_id: check `n.gender_code==f_code` AND `name(p)` contains `An`; check `aka_name__person_id` range non-empty; check `person_info__person_id` range has at least one row with `info_type_id == it3_id`.
- Build once: `cast_id`, `verified_id`, `f_code`, `us_code`, `actress_id`, `it_id`, `it3_id`, `cam_kw_id`, `chn_set` (Queen ids), `cn_us_set`, ci.note 3-literal set.
- Selectivities: title='Shrek 2' ~10^-7 of titles; chn='Queen' a few rows; cam keyword ~0.05% of movies; the global join collapses to <100 rows.
- MIN aggregation: 3 outputs (chn.name, n.name, t.title). chn.name is always `"Queen"` for surviving rows (so MIN is `"Queen"` if any match); t.title `"Shrek 2"`.
- LIKE notes:
  - `n.name LIKE '%An%'`: memmem substring on name.
  - `mi.info LIKE 'Japan:%200%'`: prefix `Japan:` then anywhere `200` (i.e. starts-with then contains).

## Indexes
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`.
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`.
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.
- `aka_name__person_id` (offsets_only): `_idx/aka_name__person_id__offsets.bin`; int32 length 4167493. Existence test: `off[p+1] > off[p]`.
- `person_info__person_id` (offsets_only): `_idx/person_info__person_id__offsets.bin`; int32 length 4167493. Use to slice pi rows for a candidate person; scan small range testing `info_type_id == it3_id`.

Usage:
```cpp
// movie-side
int32_t lo = mo_off[t_id], hi = mo_off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
// per-person pi probe
int32_t plo = pi_off[p_id], phi = pi_off[p_id + 1];
for (int32_t r = plo; r < phi; ++r)
    if (pi_itype[r] == it3_id) { found = true; break; }
```

Rules:
- Do NOT use `person_info__info_type_id` CSR here; the per-person range is tiny.
- Resolve `[us]`, `'f'` from their dict files; never hardcode codes.
- Varlen NULL = empty entry; dict NULL = code 0.
- char_name `'Queen'` may yield multiple ids — collect them all.
