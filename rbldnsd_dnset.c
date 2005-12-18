/* $Id$
 * Dataset type which consists of a set of (possible wildcarded)
 * domain names together with (A,TXT) result for each.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rbldnsd.h"

struct entry {
  const unsigned char *ldn;	/* DN key, mp-allocated, length byte first */
  const char *rr;		/* A and TXT RRs */
};

/*
 * We store all domain names in a sorted array, using binary
 * search to find an entry.  There are two similar arrays -
 * for plain entries and for wildcard entries - with all
 * variables indexed by EP and EW.
 */

struct dsdata {
  unsigned n[2]; /* number of entries */
  unsigned a[2]; /* entries allocated (used only when loading) */
  unsigned h[2]; /* hint: number of entries to allocate next time */
  struct entry *e[2]; /* entries: plain and wildcard */
  unsigned maxlab[2]; /* max level of labels */
  unsigned minlab[2]; /* min level of labels */
  const char *def_rr; /* default A and TXT RRs */
};

/* indexes */
#define EP 0			/* plain entry */
#define EW 1			/* wildcard entry */

definedstype(dnset, 0, "set of (domain name, value) pairs");

static void ds_dnset_reset(struct dsdata *dsd, int UNUSED unused_freeall) {
  unsigned h0 = dsd->h[0], h1 = dsd->h[1];
  if (dsd->e[EP]) free(dsd->e[EP]);
  if (dsd->e[EW]) free(dsd->e[EW]);
  memset(dsd, 0, sizeof(*dsd));
  dsd->minlab[EP] = dsd->minlab[EW] = DNS_MAXDN;
  dsd->h[0] = h0; dsd->h[1] = h1;
}

static void ds_dnset_start(struct dataset *ds) {
  ds->ds_dsd->def_rr = def_rr;
}

static int
ds_dnset_addent(struct dsdata *dsd, int idx,
                const unsigned char *ldn, const char *rr,
                unsigned dnlab) {
  struct entry *e;

  e = dsd->e[idx];
  if (dsd->n[idx] >= dsd->a[idx]) { /* expand array */
    dsd->a[idx] = dsd->a[idx] ? dsd->a[idx] << 1 :
                  dsd->h[idx] ? dsd->h[idx] : 64;
    e = trealloc(struct entry, e, dsd->a[idx]);
    if (!e) return 0;
    dsd->e[idx] = e;
  }

  /* fill up an entry */
  e += dsd->n[idx]++;
  e->ldn = ldn;
  e->rr = rr;

  /* adjust min/max #labels */
  if (dsd->maxlab[idx] < dnlab) dsd->maxlab[idx] = dnlab;
  if (dsd->minlab[idx] > dnlab) dsd->minlab[idx] = dnlab;

  return 1;
}

static int
ds_dnset_line(struct dataset *ds, char *s, struct dsctx *dsc) {
  struct dsdata *dsd = ds->ds_dsd;
  unsigned char dn[DNS_MAXDN];
  const char *rr;
  unsigned char *ldn;
  unsigned dnlen, size;
  int not, iswild, isplain;

  if (*s == ':') {		/* default entry */
    if (!(size = parse_a_txt(s, &rr, def_rr, dsc)))
      return 1;
    if (!(dsd->def_rr = mp_dmemdup(ds->ds_mp, rr, size)))
      return 0;
    return 1;
  }

  /* check negation */
  if (*s == '!') {
    not = 1;
    ++s; SKIPSPACE(s);
  }
  else
    not = 0;

  /* check for wildcard: .xxx or *.xxx */
  if (*s == '.') { iswild = 1; isplain = 1; ++s; }
  else if (s[0] == '*' && s[1] == '.') { iswild = 1; isplain = 0; s += 2; }
  else { iswild = 0; isplain = 1; }

  /* disallow emptry DN to be listed (i.e. "all"?) */
  if (!(s = parse_dn(s, dn, &dnlen)) || dnlen == 1) {
    dswarn(dsc, "invalid domain name");
    return 1;
  }

  dns_dntol(dn, dn);		/* lowercase */

  if (not)
    rr = NULL;			/* negation entry */
  else {			/* else parse rest */
    SKIPSPACE(s);
    if (!*s || ISCOMMENT(*s))	/* use default if none given */
      rr = dsd->def_rr;
    else if (!(size = parse_a_txt(s, &rr, dsd->def_rr, dsc)))
      return 1;
    else if (!(rr = mp_dmemdup(ds->ds_mp, rr, size)))
      return 0;
  }

  ldn = (unsigned char*)mp_alloc(ds->ds_mp, dnlen + 1, 0);
  if (!ldn)
    return 0;
  ldn[0] = (unsigned char)(dnlen - 1);
  memcpy(ldn + 1, dn, dnlen);

  dnlen = dns_dnlabels(dn);
  if (isplain && !ds_dnset_addent(dsd, EP, ldn, rr, dnlen))
    return 0;
  if (iswild && !ds_dnset_addent(dsd, EW, ldn, rr, dnlen))
    return 0;

  return 1;
}

#define min(a,b) ((a)<(b)?(a):(b))

static int ds_dnset_lt(const struct entry *a, const struct entry *b) {
  int r;
  if (a->ldn[0] < b->ldn[0]) return 1;
  if (a->ldn[0] > b->ldn[0]) return 0;
  r = memcmp(a->ldn + 1, b->ldn + 1, a->ldn[0]);
  return
     r < 0 ? 1 :
     r > 0 ? 0 :
     a->rr < b->rr;
}

static void ds_dnset_finish(struct dataset *ds, struct dsctx *dsc) {
  struct dsdata *dsd = ds->ds_dsd;
  unsigned r;
  for(r = 0; r < 2; ++r) {
    if (!dsd->n[r]) {
      dsd->h[r] = 0;
      continue;
    }
    dsd->h[r] = dsd->a[r];
    while((dsd->h[r] >> 1) >= dsd->n[r])
      dsd->h[r] >>= 1;

#   define QSORT_TYPE struct entry
#   define QSORT_BASE dsd->e[r]
#   define QSORT_NELT dsd->n[r]
#   define QSORT_LT(a,b) ds_dnset_lt(a,b)
#   include "qsort.c"

    /* we make all the same DNs point to one string for faster searches */
    { register struct entry *e, *t;
      for(e = dsd->e[r], t = e + dsd->n[r] - 1; e < t; ++e)
        if (memcmp(e[0].ldn, e[1].ldn, e[0].ldn[0] + 1) == 0)
          e[1].ldn = e[0].ldn;
    }
#define dnset_eeq(a,b) a.ldn == b.ldn && rrs_equal(a,b)
    REMOVE_DUPS(struct entry, dsd->e[r], dsd->n[r], dnset_eeq);
    SHRINK_ARRAY(struct entry, dsd->e[r], dsd->n[r], dsd->a[r]);
  }
  dsloaded(dsc, "e/w=%u/%u", dsd->n[EP], dsd->n[EW]);
}

static const struct entry *
ds_dnset_find(const struct entry *e, int n,
              const unsigned char *dn, unsigned dnlen0) {
  int a = 0, b = n - 1, m, r;

  /* binary search */
  while(a <= b) {
    /* middle entry */
    const struct entry *t = e + (m = (a + b) >> 1);
    if (t->ldn[0] < dnlen0)		/* middle entry < dn */
      a = m + 1;			/* look in last half */
    else if (t->ldn[0] > dnlen0)	/* middle entry > dn */
      b = m - 1;			/* look in first half */
    /* lengths match, compare the DN itself */
    else if ((r = memcmp(t->ldn + 1, dn, dnlen0)) == 0) {
      /* found exact match, seek back to
       * first entry with this domain name */
      dn = (t--)->ldn;
      while(t >= e && t->ldn == dn)
        --t;
      return t + 1;
    }
    else if (r < 0)			/* middle entry < dn */
      a = m + 1;			/* look in last half */
    else				/* middle entry > dn */
      b = m - 1;			/* look in first half */
  }

  return NULL;			/* not found */
}

static int
ds_dnset_query(const struct dataset *ds, const struct dnsqinfo *qi,
               struct dnspacket *pkt) {
  const struct dsdata *dsd = ds->ds_dsd;
  const unsigned char *dn = qi->qi_dn;
  unsigned qlen0 = qi->qi_dnlen0;
  unsigned qlab = qi->qi_dnlab;
  const struct entry *e, *t;
  char name[DNS_MAXDOMAIN+1];

  if (!qlab) return 0;		/* do not match empty dn */
  check_query_overwrites(qi);

  if (qlab > dsd->maxlab[EP] 	/* if we have less labels, search unnec. */
      || qlab < dsd->minlab[EP]	/* ditto for more */
      || !(e = ds_dnset_find(dsd->e[EP], dsd->n[EP], dn, qlen0))) {

    /* try wildcard */

    /* remove labels until number of labels in query is greather
     * than we have in wildcard array, but remove at least 1 label
     * for wildcard itself. */
    do
      --qlab, qlen0 -= *dn + 1, dn += *dn + 1;
    while(qlab > dsd->maxlab[EW]);

    /* now, lookup every so long dn in wildcard array */
    for(;;) {

      if (qlab < dsd->minlab[EW])
        /* oh, number of labels in query become less than
         * minimum we have listed.  Nothing to search anymore */
        return 0;

      if ((e = ds_dnset_find(dsd->e[EW], dsd->n[EW], dn, qlen0)))
        break;			/* found, listed */

      /* remove next label at the end of rdn */
      qlen0 -= *dn + 1;
      dn += *dn + 1;
      --qlab;

    }
    t = dsd->e[EW] + dsd->n[EW];

  }
  else
    t = dsd->e[EP] + dsd->n[EP];

  if (!e->rr) return 0;	/* exclusion */

  dn = e->ldn;
  if (qi->qi_tflag & NSQUERY_TXT)
    dns_dntop(e->ldn + 1, name, sizeof(name));
  do addrr_a_txt(pkt, qi->qi_tflag, e->rr, name, ds);
  while(++e < t && e->ldn == dn);

  return NSQUERY_FOUND;
}

#ifndef NO_MASTER_DUMP

static void
ds_dnset_dump(const struct dataset *ds,
              const unsigned char UNUSED *unused_odn,
              FILE *f) {
  const struct dsdata *dsd = ds->ds_dsd;
  const struct entry *e, *t;
  unsigned char name[DNS_MAXDOMAIN+4];
  for (e = dsd->e[EP], t = e + dsd->n[EP]; e < t; ++e) {
    dns_dntop(e->ldn + 1, name, sizeof(name));
    dump_a_txt(name, e->rr, name, ds, f);
  }
  name[0] = '*'; name[1] = '.';
  for (e = dsd->e[EW], t = e + dsd->n[EW]; e < t; ++e) {
    dns_dntop(e->ldn + 1, name + 2, sizeof(name) - 2);
    dump_a_txt(name, e->rr, name + 2, ds, f);
  }
}

#endif
