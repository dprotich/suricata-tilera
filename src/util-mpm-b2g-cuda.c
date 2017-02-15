/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 * \author Martin Beyer <martin.beyer@marasystems.de>
 */

#include "suricata-common.h"
#include "suricata.h"
#include "detect.h"
#include "detect-engine-mpm.h"
#include "util-bloomfilter.h"
#include "util-mpm-b2g-cuda.h"
#include "util-mpm.h"
#include "util-print.h"

#include "threadvars.h"

#include "util-error.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "conf.h"

#include "util-cuda-handlers.h"
#include "util-cuda.h"
#include "util-cpu.h"
#include "tm-threads.h"
#include "threads.h"
#include "tmqh-simple.h"

#include "detect-engine-address.h"
#include "detect-engine-port.h"
#include "detect-engine.h"
#include "detect-parse.h"

#include "cuda-packet-batcher.h"

/* macros decides if cuda is enabled for the platform or not */
#ifdef __SC_CUDA_SUPPORT__

#define INIT_HASH_SIZE 65536

#ifdef B2G_CUDA_COUNTERS
#define COUNT(counter) (counter)
#else
#define COUNT(counter)
#endif /* B2G_CUDA_COUNTERS */

static uint32_t b2g_hash_size = 0;
static uint32_t b2g_bloom_size = 0;
static void *b2g_func;

/* threadvars Cuda(C) Mpm(M) B2G(B) Rules(R) Content(C) */
ThreadVars *tv_CMB2_RC = NULL;

void B2gCudaInitCtx(MpmCtx *, int);
void B2gCudaThreadInitCtx(ThreadVars *, MpmCtx *, MpmThreadCtx *, uint32_t);
void B2gCudaDestroyCtx(MpmCtx *);
void B2gCudaThreadDestroyCtx(MpmCtx *, MpmThreadCtx *);
int B2gCudaAddPatternCI(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t,
                        uint32_t, uint32_t, uint8_t);
int B2gCudaAddPatternCS(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t,
                        uint32_t, uint32_t, uint8_t);
int B2gCudaPreparePatterns(MpmCtx *mpm_ctx);
uint32_t B2gCudaSearchWrap(MpmCtx *, MpmThreadCtx *,
                                  PatternMatcherQueue *, uint8_t *,
                                  uint16_t);
uint32_t B2gCudaSearch1(MpmCtx *, MpmThreadCtx *, PatternMatcherQueue *,
                        uint8_t *, uint16_t);
#ifdef B2G_CUDA_SEARCH2
uint32_t B2gCudaSearch2(MpmCtx *, MpmThreadCtx *, PatternMatcherQueue *,
                        uint8_t *, uint16_t);
#endif
uint32_t B2gCudaSearch(MpmCtx *, MpmThreadCtx *, PatternMatcherQueue *,
                       uint8_t *, uint16_t);
uint32_t B2gCudaSearchBNDMq(MpmCtx *, MpmThreadCtx *, PatternMatcherQueue *,
                            uint8_t *, uint16_t);
void B2gCudaPrintInfo(MpmCtx *);
void B2gCudaPrintSearchStats(MpmThreadCtx *);
void B2gCudaRegisterTests(void);

/**
 * \brief Register the CUDA B2g Mpm.
 */
void MpmB2gCudaRegister(void)
{
    mpm_table[MPM_B2G_CUDA].name = "b2g_cuda";
    mpm_table[MPM_B2G_CUDA].max_pattern_length = B2G_CUDA_WORD_SIZE;
    mpm_table[MPM_B2G_CUDA].InitCtx = B2gCudaInitCtx;
    mpm_table[MPM_B2G_CUDA].InitThreadCtx = B2gCudaThreadInitCtx;
    mpm_table[MPM_B2G_CUDA].DestroyCtx = B2gCudaDestroyCtx;
    mpm_table[MPM_B2G_CUDA].DestroyThreadCtx = B2gCudaThreadDestroyCtx;
    mpm_table[MPM_B2G_CUDA].AddPattern = B2gCudaAddPatternCS;
    mpm_table[MPM_B2G_CUDA].AddPatternNocase = B2gCudaAddPatternCI;
    mpm_table[MPM_B2G_CUDA].Prepare = B2gCudaPreparePatterns;
    mpm_table[MPM_B2G_CUDA].Search = B2gCudaSearchWrap;
    mpm_table[MPM_B2G_CUDA].Cleanup = NULL;
    mpm_table[MPM_B2G_CUDA].PrintCtx = B2gCudaPrintInfo;
    mpm_table[MPM_B2G_CUDA].PrintThreadCtx = B2gCudaPrintSearchStats;
    mpm_table[MPM_B2G_CUDA].RegisterUnittests = B2gCudaRegisterTests;
}

void B2gCudaPrintInfo(MpmCtx *mpm_ctx)
{
#ifdef DEBUG
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    SCLogDebug("MPM B2g Cuda Information:");
    SCLogDebug("Memory allocs:    %" PRIu32, mpm_ctx->memory_cnt);
    SCLogDebug("Memory alloced:   %" PRIu32, mpm_ctx->memory_size);
    SCLogDebug(" Sizeofs:");
    SCLogDebug("  MpmCtx          %" PRIuMAX, (uintmax_t)sizeof(MpmCtx));
    SCLogDebug("  B2gCudaCtx      %" PRIuMAX, (uintmax_t)sizeof(B2gCudaCtx));
    SCLogDebug("  B2gCudaPattern  %" PRIuMAX, (uintmax_t)sizeof(B2gCudaPattern));
    SCLogDebug("  B2gCudaHashItem %" PRIuMAX, (uintmax_t)sizeof(B2gCudaHashItem));
    SCLogDebug("Unique Patterns:  %" PRIu32, mpm_ctx->pattern_cnt);
    SCLogDebug("Smallest:         %" PRIu32, mpm_ctx->minlen);
    SCLogDebug("Largest:          %" PRIu32, mpm_ctx->maxlen);
    SCLogDebug("Hash size:        %" PRIu32, ctx->hash_size);
#endif

    return;
}

static inline B2gCudaPattern *B2gCudaAllocPattern(MpmCtx *mpm_ctx)
{
    B2gCudaPattern *p = SCMalloc(sizeof(B2gCudaPattern));
    if (unlikely(p == NULL))
        return NULL;
    memset(p, 0, sizeof(B2gCudaPattern));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gCudaPattern);

    return p;
}

static inline B2gCudaHashItem *B2gCudaAllocHashItem(MpmCtx *mpm_ctx)
{
    B2gCudaHashItem *hi = SCMalloc(sizeof(B2gCudaHashItem));
    if (unlikely(hi == NULL))
        return NULL;
    memset(hi, 0, sizeof(B2gCudaHashItem));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gCudaHashItem);

    return hi;
}

static void B2gCudaHashFree(MpmCtx *mpm_ctx, B2gCudaHashItem *hi)
{
    if (hi == NULL)
        return;

    B2gCudaHashItem *t = hi->nxt;
    B2gCudaHashFree(mpm_ctx, t);

    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B2gCudaHashItem);
    SCFree(hi);

    return;
}

static inline void memcpy_tolower(uint8_t *d, uint8_t *s, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++)
        d[i] = u8_tolower(s[i]);

    return;
}

static inline uint32_t B2gCudaInitHash(B2gCudaPattern *p)
{
    uint32_t hash = p->len * p->original_pat[0];
    if (p->len > 1)
        hash += p->original_pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline uint32_t B2gCudaInitHashRaw(uint8_t *pat, uint16_t patlen)
{
    uint32_t hash = patlen * pat[0];
    if (patlen > 1)
        hash += pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline int B2gCudaInitHashAdd(B2gCudaCtx *ctx, B2gCudaPattern *p)
{
    uint32_t hash = B2gCudaInitHash(p);

    if (ctx->init_hash[hash] == NULL) {
        ctx->init_hash[hash] = p;
        return 0;
    }

    B2gCudaPattern *tt = NULL;
    B2gCudaPattern *t = ctx->init_hash[hash];

    /* get the list tail */
    do {
        tt = t;
        t = t->next;
    } while (t != NULL);

    tt->next = p;

    return 0;
}

static inline int B2gCudaCmpPattern(B2gCudaPattern *p, uint8_t *pat,
                                    uint16_t patlen, char flags)
{
    if (p->len != patlen)
        return 0;

    if (p->flags != flags)
        return 0;

    if (memcmp(p->cs, pat, patlen) != 0)
        return 0;

    return 1;
}

static inline B2gCudaPattern *B2gCudaInitHashLookup(B2gCudaCtx *ctx, uint8_t *pat,
                                                    uint16_t patlen, char flags,
                                                    uint32_t pid)
{
    uint32_t hash = B2gCudaInitHashRaw(pat, patlen);

    if (ctx->init_hash[hash] == NULL)
        return NULL;

    B2gCudaPattern *t = ctx->init_hash[hash];
    for ( ; t != NULL; t = t->next) {
        //if (B2gCudaCmpPattern(t, pat, patlen, flags) == 1)
        if (t->flags == flags && t->id == pid)
            return t;
    }

    return NULL;
}

void B2gCudaFreePattern(MpmCtx *mpm_ctx, B2gCudaPattern *p)
{
    if (p != NULL && p->cs != NULL && p->cs != p->ci) {
        SCFree(p->cs);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p != NULL && p->ci != NULL) {
        SCFree(p->ci);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p != NULL) {
        SCFree(p);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= sizeof(B2gCudaPattern);
    }

    return;
}

static inline int B2gCudaAddPattern(MpmCtx *mpm_ctx, uint8_t *pat,
                                    uint16_t patlen, uint16_t offset,
                                    uint16_t depth, uint32_t pid,
                                    uint32_t sid, uint8_t flags)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;

    SCLogDebug("ctx %p len %"PRIu16" pid %" PRIu32, ctx, patlen, pid);

    if (patlen == 0)
        return 0;

    /* get a memory piece */
    B2gCudaPattern *p = B2gCudaInitHashLookup(ctx, pat, patlen, flags, pid);
    if (p == NULL) {
        SCLogDebug("allocing new pattern");

        p = B2gCudaAllocPattern(mpm_ctx);
        if (p == NULL)
            goto error;

        p->len = patlen;
        p->flags = flags;
        p->id = pid;

        p->original_pat = SCMalloc(patlen);
        if (p->original_pat == NULL)
            goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy(p->original_pat, pat, patlen);

        /* setup the case insensitive part of the pattern */
        p->ci = SCMalloc(patlen);
        if (p->ci == NULL)
            goto error;

        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy_tolower(p->ci, pat, patlen);

        /* setup the case sensitive part of the pattern */
        if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
            /* nocase means no difference between cs and ci */
            p->cs = p->ci;
        } else {
            if (memcmp(p->ci,pat,p->len) == 0) {
                /* no diff between cs and ci: pat is lowercase */
                p->cs = p->ci;
            } else {
                p->cs = SCMalloc(patlen);
                if (p->cs == NULL)
                    goto error;

                mpm_ctx->memory_cnt++;
                mpm_ctx->memory_size += patlen;
                memcpy(p->cs, pat, patlen);
            }
        }

        //printf("B2gAddPattern: ci \""); prt(p->ci,p->len);
        //printf("\" cs \""); prt(p->cs,p->len);
        //printf("\"\n");

        /* put in the pattern hash */
        B2gCudaInitHashAdd(ctx, p);

        if (mpm_ctx->pattern_cnt == 65535) {
            printf("Max search words reached\n");
            exit(1);
        }

        mpm_ctx->pattern_cnt++;

        if (mpm_ctx->maxlen < patlen) mpm_ctx->maxlen = patlen;
        if (mpm_ctx->minlen == 0) mpm_ctx->minlen = patlen;
        else if (mpm_ctx->minlen > patlen) mpm_ctx->minlen = patlen;
    }

    return 0;

error:
    B2gCudaFreePattern(mpm_ctx, p);
    return -1;
}

int B2gCudaAddPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
                        uint16_t offset, uint16_t depth, uint32_t pid,
                        uint32_t sid, uint8_t flags)
{
    flags |= MPM_PATTERN_FLAG_NOCASE;
    return B2gCudaAddPattern(mpm_ctx, pat, patlen, offset, depth, pid, sid, flags);
}

int B2gCudaAddPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
                        uint16_t offset, uint16_t depth, uint32_t pid,
                        uint32_t sid, uint8_t flags)
{
    return B2gCudaAddPattern(mpm_ctx, pat, patlen, offset, depth, pid, sid, flags);
}

static inline uint32_t B2gCudaBloomHash(void *data, uint16_t datalen, uint8_t iter,
                                        uint32_t hash_size)
{
     uint8_t *d = (uint8_t *)data;
     uint16_t i;
     uint32_t hash = (uint32_t)u8_tolower(*d);

     for (i = 1; i < datalen; i++) {
         d++;
         hash += (u8_tolower(*d)) ^ i;
     }
     hash <<= (iter+1);
     hash %= hash_size;

     return hash;
}

static void B2gCudaPrepareHash(MpmCtx *mpm_ctx)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    uint16_t i = 0;
    uint16_t idx = 0;
    uint8_t idx8 = 0;

    ctx->hash = (B2gCudaHashItem **)SCMalloc(sizeof(B2gCudaHashItem *) *
                                             ctx->hash_size);
    if (ctx->hash == NULL)
        goto error;
    memset(ctx->hash, 0, sizeof(B2gCudaHashItem *) * ctx->hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2gCudaHashItem *) * ctx->hash_size);

#ifdef B2G_CUDA_SEARCH2
    ctx->hash2 = (B2gCudaHashItem **)SCMalloc(sizeof(B2gCudaHashItem *) *
                                              ctx->hash_size);
    if (ctx->hash2 == NULL)
        goto error;
    memset(ctx->hash2, 0, sizeof(B2gCudaHashItem *) * ctx->hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2gCudaHashItem *) * ctx->hash_size);
#endif

    /* alloc the pminlen array */
    ctx->pminlen = (uint8_t *)SCMalloc(sizeof(uint8_t) * ctx->hash_size);
    if (ctx->pminlen == NULL)
        goto error;
    memset(ctx->pminlen, 0, sizeof(uint8_t) * ctx->hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(uint8_t) * ctx->hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        if(ctx->parray[i]->len == 1) {
            idx8 = (uint8_t)ctx->parray[i]->ci[0];
            if (ctx->hash1[idx8].flags == 0) {
                ctx->hash1[idx8].idx = i;
                ctx->hash1[idx8].flags |= 0x01;
            } else {
                B2gCudaHashItem *hi = B2gCudaAllocHashItem(mpm_ctx);
                if (hi == NULL)
                    goto error;
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B2gCudaHashItem *thi = &ctx->hash1[idx8];
                while (thi->nxt)
                    thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->pat_1_cnt++;
#ifdef B2G_CUDA_SEARCH2
        } else if(ctx->parray[i]->len == 2) {
            idx = B2G_CUDA_HASH16(ctx->parray[i]->ci[0], ctx->parray[i]->ci[1]);
            if (ctx->hash2[idx] == NULL) {
                B2gCudaHashItem *hi = B2gCudaAllocHashItem(mpm_ctx);
                if (hi == NULL)
                    goto error;
                hi->idx = i;
                hi->flags |= 0x01;

                ctx->hash2[idx] = hi;
            } else {
                B2gCudaHashItem *hi = B2gCudaAllocHashItem(mpm_ctx);
                if (hi == NULL)
                    goto error;
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B2gCudaHashItem *thi = ctx->hash2[idx];
                while (thi->nxt)
                    thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->pat_2_cnt++;
#endif
        } else {
            idx = B2G_CUDA_HASH16(ctx->parray[i]->ci[ctx->m - 2],
                             ctx->parray[i]->ci[ctx->m - 1]);
            SCLogDebug("idx %" PRIu32 ", %c.%c", idx, ctx->parray[i]->ci[ctx->m - 2],
                       ctx->parray[i]->ci[ctx->m - 1]);

            if (ctx->hash[idx] == NULL) {
                B2gCudaHashItem *hi = B2gCudaAllocHashItem(mpm_ctx);
                if (hi == NULL)
                    goto error;
                hi->idx = i;
                hi->flags |= 0x01;
                ctx->pminlen[idx] = ctx->parray[i]->len;

                ctx->hash[idx] = hi;
            } else {
                B2gCudaHashItem *hi = B2gCudaAllocHashItem(mpm_ctx);
                if (hi == NULL)
                    goto error;
                hi->idx = i;
                hi->flags |= 0x01;

                if (ctx->parray[i]->len < ctx->pminlen[idx])
                    ctx->pminlen[idx] = ctx->parray[i]->len;

                /* Append this HashItem to the list */
                B2gCudaHashItem *thi = ctx->hash[idx];
                while (thi->nxt)
                    thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->pat_x_cnt++;
        }
    }

    /* alloc the bloom array */
    ctx->bloom = (BloomFilter **)SCMalloc(sizeof(BloomFilter *) * ctx->hash_size);
    if (ctx->bloom == NULL)
        goto error;
    memset(ctx->bloom, 0, sizeof(BloomFilter *) * ctx->hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(BloomFilter *) * ctx->hash_size);

    uint32_t h;
    for (h = 0; h < ctx->hash_size; h++) {
        B2gCudaHashItem *hi = ctx->hash[h];
        if (hi == NULL)
            continue;

        ctx->bloom[h] = BloomFilterInit(b2g_bloom_size, 2, B2gCudaBloomHash);
        if (ctx->bloom[h] == NULL)
            continue;

        mpm_ctx->memory_cnt += BloomFilterMemoryCnt(ctx->bloom[h]);
        mpm_ctx->memory_size += BloomFilterMemorySize(ctx->bloom[h]);

        if (ctx->pminlen[h] > 8)
            ctx->pminlen[h] = 8;

        B2gCudaHashItem *thi = hi;
        do {
            SCLogDebug("adding \"%c%c\" to the bloom", ctx->parray[thi->idx]->ci[0],
                       ctx->parray[thi->idx]->ci[1]);
            BloomFilterAdd(ctx->bloom[h], ctx->parray[thi->idx]->ci,
                           ctx->pminlen[h]);
            thi = thi->nxt;
        } while (thi != NULL);
    }

    return;

error:
    return;
}

int B2gCudaBuildMatchArray(MpmCtx *mpm_ctx)
{
    SCEnter();
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;

    ctx->B2G = SCMalloc(sizeof(B2G_CUDA_TYPE) * ctx->hash_size);
    if (ctx->B2G == NULL)
        return -1;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2G_CUDA_TYPE) * ctx->hash_size);

    memset(ctx->B2G, 0, b2g_hash_size * sizeof(B2G_CUDA_TYPE));

    uint32_t j;
    uint32_t a;

    /* fill the match array */
    for (j = 0; j <= (ctx->m - B2G_CUDA_Q); j++) {
        for (a = 0; a < mpm_ctx->pattern_cnt; a++) {
            if (ctx->parray[a]->len < ctx->m)
                continue;

            uint16_t h = B2G_CUDA_HASH16(u8_tolower(ctx->parray[a]->ci[j]),
                                         u8_tolower(ctx->parray[a]->ci[j + 1]));
            ctx->B2G[h] = ctx->B2G[h] | (1 << (ctx->m - j));

            SCLogDebug("h %" PRIu16 ", ctx->B2G[h] %" PRIu32, h, ctx->B2G[h]);
        }
    }

    ctx->s0 = 1;

    SCReturnInt(0);
}

int B2gCudaSetDeviceBuffers(MpmCtx *mpm_ctx)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;

    /* search kernel */
    if (SCCudaMemAlloc(&ctx->cuda_B2G,
                       sizeof(B2G_CUDA_TYPE) * ctx->hash_size) == -1) {
        goto error;
    }
    if (SCCudaMemcpyHtoD(ctx->cuda_B2G, ctx->B2G,
                         sizeof(B2G_CUDA_TYPE) * ctx->hash_size) == -1) {
        goto error;
    }

    return 0;

 error:
    return -1;
}

int B2gCudaSetKernelArgs(MpmCtx *mpm_ctx)
{
    return 0;
}

int B2gCudaPreparePatterns(MpmCtx *mpm_ctx)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;

    if (mpm_ctx->pattern_cnt == 0 || ctx->init_hash == NULL) {
        SCLogDebug("no patterns supplied to this mpm_ctx");
        return 0;
    }

    /* alloc the pattern array */
    ctx->parray = (B2gCudaPattern **)SCMalloc(mpm_ctx->pattern_cnt *
                                              sizeof(B2gCudaPattern *));
    if (ctx->parray == NULL)
        goto error;
    memset(ctx->parray, 0, mpm_ctx->pattern_cnt * sizeof(B2gCudaPattern *));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (mpm_ctx->pattern_cnt * sizeof(B2gCudaPattern *));

    /* populate it with the patterns in the hash */
    uint32_t i = 0, p = 0;
    for (i = 0; i < INIT_HASH_SIZE; i++) {
        B2gCudaPattern *node = ctx->init_hash[i];
        B2gCudaPattern *nnode = NULL;
        for ( ; node != NULL; ) {
            nnode = node->next;
            node->next = NULL;

            ctx->parray[p] = node;

            p++;
            node = nnode;
        }
    }
    /* we no longer need the hash, so free it's memory */
    SCFree(ctx->init_hash);
    ctx->init_hash = NULL;

    /* set 'm' to the smallest pattern size */
    ctx->m = mpm_ctx->minlen;

    /* make sure 'm' stays in bounds
       m can be max WORD_SIZE - 1 */
    if (ctx->m >= B2G_CUDA_WORD_SIZE) {
        ctx->m = B2G_CUDA_WORD_SIZE - 1;
    }
    if (ctx->m < 2)
        ctx->m = 2;

    ctx->hash_size = b2g_hash_size;
    B2gCudaPrepareHash(mpm_ctx);
    B2gCudaBuildMatchArray(mpm_ctx);

    if (B2gCudaSetDeviceBuffers(mpm_ctx) == -1)
        goto error;

    if (B2gCudaSetKernelArgs(mpm_ctx) == -1)
        goto error;

    SCLogDebug("ctx->pat_1_cnt %" PRIu16, ctx->pat_1_cnt);
    if (ctx->pat_1_cnt) {
        ctx->Search = B2gCudaSearch1;
#ifdef B2G_CUDA_SEARCH2
        ctx->Search = B2gCudaSearch2;
        if (ctx->pat_2_cnt) {
            ctx->MBSearch2 = B2gCudaSearch2;
        }
#endif
        ctx->MBSearch = b2g_func;
#ifdef B2G_SEARCH2
    } else if (ctx->pat_2_cnt) {
        ctx->Search = B2gSearch2;
        ctx->MBSearch = b2g_cuda_func;
#endif
    }

    return 0;

 error:
    return -1;
}

void B2gCudaPrintSearchStats(MpmThreadCtx *mpm_thread_ctx)
{
#ifdef B2G_CUDA_COUNTERS
    B2gCudaThreadCtx *tctx = (B2gCudaThreadCtx *)mpm_thread_ctx->ctx;

    printf("B2g Thread Search stats (tctx %p)\n", tctx);
    printf("Total calls: %" PRIu32 "\n", tctx->stat_calls);
    printf("Avg m/search: %0.2f\n", (tctx->stat_calls ?
                                     (float)((float)tctx->stat_m_total /
                                             (float)tctx->stat_calls) : 0));
    printf("D != 0 (possible match): %" PRIu32 "\n", tctx->stat_d0);
    printf("Avg hash items per bucket %0.2f (%" PRIu32 ")\n",
           tctx->stat_d0 ? (float)((float)tctx->stat_d0_hashloop /
                                   (float)tctx->stat_d0) : 0,
           tctx->stat_d0_hashloop);
    printf("Loop match: %" PRIu32 "\n", tctx->stat_loop_match);
    printf("Loop no match: %" PRIu32 "\n", tctx->stat_loop_no_match);
    printf("Num shifts: %" PRIu32 "\n", tctx->stat_num_shift);
    printf("Total shifts: %" PRIu32 "\n", tctx->stat_total_shift);
    printf("Avg shifts: %0.2f\n", (tctx->stat_num_shift ?
                                   (float)((float)tctx->stat_total_shift /
                                           (float)tctx->stat_num_shift)) : 0);
    printf("Total BloomFilter checks: %" PRIu32 "\n", tctx->stat_bloom_calls);
    printf("BloomFilter hits: %0.4f%% (%" PRIu32 ")\n",
           (tctx->stat_bloom_calls ? ((float)tctx->stat_bloom_hits /
                                      (float)tctx->stat_bloom_calls) * 100) : 0,
           tctx->stat_bloom_hits);
    printf("Avg pminlen: %0.2f\n\n",
           (tctx->stat_pminlen_calls ? ((float)tctx->stat_pminlen_total /
                                        (float)tctx->stat_pminlen_calls)) : 0);
#endif /* B2G_CUDA_COUNTERS */

}

static inline int memcmp_lowercase(uint8_t *s1, uint8_t *s2, uint16_t n)
{
    size_t i;

    /* check backwards because we already tested the first
     * 2 to 4 chars. This way we are more likely to detect
     * a miss and thus speed up a little... */
    for (i = n - 1; i; i--) {
        if (u8_tolower(*(s2 + i)) != s1[i])
            return 1;
    }

    return 0;
}

/**
 * \brief   Function to get the user defined values for b2g algorithm from the
 *          config file 'suricata.yaml'
 */
static void B2gGetConfig()
{
    ConfNode *b2g_conf;
    const char *hash_val = NULL;
    const char *bloom_val = NULL;
    const char *algo = NULL;

    /* init defaults */
    b2g_hash_size = HASHSIZE_LOW;
    b2g_bloom_size = BLOOMSIZE_MEDIUM;
    b2g_func = B2G_CUDA_SEARCHFUNC;

    ConfNode *pm = ConfGetNode("pattern-matcher");

    if (pm != NULL) {

        TAILQ_FOREACH(b2g_conf, &pm->head, next) {
            if (strncmp(b2g_conf->val, "b2g", 3) == 0) {

                algo = ConfNodeLookupChildValue
                        (b2g_conf->head.tqh_first, "algo");
                hash_val = ConfNodeLookupChildValue
                        (b2g_conf->head.tqh_first, "hash_size");
                bloom_val = ConfNodeLookupChildValue
                        (b2g_conf->head.tqh_first, "bf_size");

                if (algo != NULL) {
                    if (strcmp(algo, "B2gSearch") == 0) {
                        b2g_func = B2gCudaSearch;
                    } else if (strcmp(algo, "B2gSearchBNDMq") == 0) {
                        b2g_func = B2gCudaSearchBNDMq;
                    }
                }

                if (hash_val != NULL)
                    b2g_hash_size = MpmGetHashSize(hash_val);

                if (bloom_val != NULL)
                    b2g_bloom_size = MpmGetBloomSize(bloom_val);

                SCLogDebug("hash size is %"PRIu32" and bloom size is %"PRIu32"",
                b2g_hash_size, b2g_bloom_size);
            }
        }
    }
}

void B2gCudaInitCtx(MpmCtx *mpm_ctx, int module_handle)
{
    if (mpm_ctx->ctx != NULL)
        return;

    SCLogDebug("mpm_ctx %p, ctx %p", mpm_ctx, mpm_ctx->ctx);

    mpm_ctx->ctx = SCMalloc(sizeof(B2gCudaCtx));
    if (mpm_ctx->ctx == NULL)
        return;

    memset(mpm_ctx->ctx, 0, sizeof(B2gCudaCtx));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gCudaCtx);

    /* initialize the hash we use to speed up pattern insertions */
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;

    /* hold the cuda module handle against which we are registered.  This is our
     * only reference to know our place of birth */
    ctx->module_handle = module_handle;

    ctx->init_hash = SCMalloc(sizeof(B2gCudaPattern *) * INIT_HASH_SIZE);
    if (ctx->init_hash == NULL)
        return;

    memset(ctx->init_hash, 0, sizeof(B2gCudaPattern *) * INIT_HASH_SIZE);

    /* Initialize the defaults value from the config file. The given check make
       sure that we query config file only once for config values */
    if (b2g_hash_size == 0)
        B2gGetConfig();

    /* init defaults search functions */
    ctx->Search = b2g_func;

    return;
}

void B2gCudaDestroyCtx(MpmCtx *mpm_ctx)
{
    SCLogDebug("mpm_ctx %p", mpm_ctx);

    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    if (ctx == NULL)
        return;

    if (ctx->init_hash) {
        SCFree(ctx->init_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (INIT_HASH_SIZE * sizeof(B2gCudaPattern *));
    }

    if (ctx->parray) {
        uint32_t i;
        for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
            if (ctx->parray[i] != NULL) {
                B2gCudaFreePattern(mpm_ctx, ctx->parray[i]);
            }
        }

        SCFree(ctx->parray);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(B2gCudaPattern));
    }

    if (ctx->B2G) {
        SCFree(ctx->B2G);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2G_CUDA_TYPE) * ctx->hash_size);
    }

    if (ctx->bloom) {
        uint32_t h;
        for (h = 0; h < ctx->hash_size; h++) {
            if (ctx->bloom[h] == NULL)
                continue;

            mpm_ctx->memory_cnt -= BloomFilterMemoryCnt(ctx->bloom[h]);
            mpm_ctx->memory_size -= BloomFilterMemorySize(ctx->bloom[h]);

            BloomFilterFree(ctx->bloom[h]);
        }

        SCFree(ctx->bloom);

        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(BloomFilter *) * ctx->hash_size);
    }

    if (ctx->hash) {
        uint32_t h;
        for (h = 0; h < ctx->hash_size; h++) {
            if (ctx->hash[h] == NULL)
                continue;

            B2gCudaHashFree(mpm_ctx, ctx->hash[h]);
        }

        SCFree(ctx->hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2gCudaHashItem) * ctx->hash_size);
    }

    if (ctx->pminlen) {
        SCFree(ctx->pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(uint8_t) * ctx->hash_size);
    }

    CUcontext dummy_context;
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(ctx->module_handle);
    if (module_data == NULL) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "How did we even fail to get a "
                   "module_data if we are having a module_handle");
        goto error;
    }
    if (SCCudaHlGetCudaContext(&dummy_context, "mpm", ctx->module_handle) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda context for the "
                   "module %s", module_data->name);
        goto error;
    }
    SCCudaCtxPushCurrent(dummy_context);

    if (ctx->cuda_B2G != 0) {
        if (SCCudaMemFree(ctx->cuda_B2G) == -1) {
            SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error freeing ctx->cuda_B2G ");
            goto error;
        }
        ctx->cuda_B2G = 0;
    }
    SCCudaCtxPopCurrent(&dummy_context);

    SCFree(mpm_ctx->ctx);
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B2gCudaCtx);


 error:
    return;
}

void B2gCudaThreadInitCtx(ThreadVars *tv, MpmCtx *mpm_ctx,
                          MpmThreadCtx *mpm_thread_ctx, uint32_t matchsize)
{
    memset(mpm_thread_ctx, 0, sizeof(MpmThreadCtx));

    /* size can be null when optimized */
    if (sizeof(B2gCudaThreadCtx) > 0) {
        mpm_thread_ctx->ctx = SCThreadMalloc(tv, sizeof(B2gCudaThreadCtx));
        if (mpm_thread_ctx->ctx == NULL)
            return;

        memset(mpm_thread_ctx->ctx, 0, sizeof(B2gCudaThreadCtx));

        mpm_thread_ctx->memory_cnt++;
        mpm_thread_ctx->memory_size += sizeof(B2gCudaThreadCtx);
    }

    return;
}

void B2gCudaThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx)
{
    B2gCudaThreadCtx *ctx = (B2gCudaThreadCtx *)mpm_thread_ctx->ctx;

    B2gCudaPrintSearchStats(mpm_thread_ctx);

    /* can be NULL if B2gThreadCtx is optimized to 0 */
    if (ctx != NULL) {
        mpm_thread_ctx->memory_cnt--;
        mpm_thread_ctx->memory_size -= sizeof(B2gCudaThreadCtx);
        SCFree(mpm_thread_ctx->ctx);
    }

    return;
}

inline uint32_t B2gCudaSearchWrap(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                                  PatternMatcherQueue *pmq, uint8_t *buf,
                                  uint16_t buflen)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    return ctx ? ctx->Search(mpm_ctx, mpm_thread_ctx, pmq, buf, buflen) : 0;
}

uint32_t B2gCudaSearchBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                            PatternMatcherQueue *pmq, uint8_t *buf,
                            uint16_t buflen)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
#ifdef B2G_COUNTERS
    B2gCudaThreadCtx *tctx = (B2gCudaThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = ctx->m - B2G_CUDA_Q + 1, matches = 0;
    B2G_CUDA_TYPE d;

    //printf("\n");
    //PrintRawDataFp(stdout, buf, buflen);

    SCLogDebug("buflen %"PRIu16", ctx->m %"PRIu32", pos %"PRIu32"", buflen,
            ctx->m, pos);

    COUNT(tctx->stat_calls++);
    COUNT(tctx->stat_m_total+=ctx->m);

    if (buflen < ctx->m)
        return 0;

    while (pos <= (uint32_t)(buflen - B2G_CUDA_Q + 1)) {
        uint16_t h = B2G_CUDA_HASH16(u8_tolower(buf[pos - 1]),u8_tolower(buf[pos]));
        d = ctx->B2G[h];

        if (d != 0) {
            COUNT(tctx->stat_d0++);
            uint32_t j = pos;
            uint32_t first = pos - (ctx->m - B2G_CUDA_Q + 1);

            do {
                j = j - 1;

                if (d >= (uint32_t)(1 << (ctx->m - 1))) {
                    if (j > first) pos = j;
                    else {
                        /* get our patterns from the hash */
                        h = B2G_CUDA_HASH16(u8_tolower(buf[j + ctx->m - 2]),u8_tolower(buf[j + ctx->m - 1]));

                        if (ctx->bloom[h] != NULL) {
                            COUNT(tctx->stat_pminlen_calls++);
                            COUNT(tctx->stat_pminlen_total+=ctx->pminlen[h]);

                            if ((buflen - j) < ctx->pminlen[h]) {
                                goto skip_loop;
                            } else {
                                COUNT(tctx->stat_bloom_calls++);

                                if (BloomFilterTest(ctx->bloom[h], buf+j, ctx->pminlen[h]) == 0) {
                                    COUNT(tctx->stat_bloom_hits++);

                                    SCLogDebug("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "",
                                        ctx->bloom[h], buflen, pos, ctx->pminlen[h]);
                                    goto skip_loop;
                                }
                            }
                        }

                        B2gCudaHashItem *hi = ctx->hash[h], *thi;
                        for (thi = hi; thi != NULL; thi = thi->nxt) {
                            COUNT(tctx->stat_d0_hashloop++);
                            B2gCudaPattern *p = ctx->parray[thi->idx];

                            if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                                if ((buflen - j) < p->len) {
                                    continue;
                                }

                                if (memcmp_lowercase(p->ci, buf+j, p->len) == 0) {
#ifdef PRINTMATCH
                                    printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
#endif
                                    COUNT(tctx->stat_loop_match++);

                                    matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                                } else {
                                    COUNT(tctx->stat_loop_no_match++);
                                }
                            } else {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp(p->cs, buf+j, p->len) == 0) {
#ifdef PRINTMATCH
                                    printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
#endif
                                    COUNT(tctx->stat_loop_match++);

                                    matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                                } else {
                                    COUNT(tctx->stat_loop_no_match++);
                                }
                            }
                        }
skip_loop:
                        SCLogDebug("skipped");
                        //SCLogDebug("output at pos %" PRIu32 ": ", j); prt(buf + (j), ctx->m); printf("\n");
                        ;
                    }
                }

                if (j == 0) {
                    break;
                }

                h = B2G_CUDA_HASH16(u8_tolower(buf[j - 1]),u8_tolower(buf[j]));
                d = (d << 1) & ctx->B2G[h];
            } while (d != 0);
        }
        COUNT(tctx->stat_num_shift++);
        COUNT(tctx->stat_total_shift += (ctx->m - B2G_Q + 1));
        pos = pos + ctx->m - B2G_CUDA_Q + 1;

        SCLogDebug("pos %"PRIu32"", pos);
    }

    SCLogDebug("matches %"PRIu32"", matches);
    return matches;
}

uint32_t B2gCudaSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                       PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
#ifdef B2G_CUDA_COUNTERS
    B2gCudaThreadCtx *tctx = (B2gCudaThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = 0, matches = 0;
    B2G_CUDA_TYPE d;
    uint32_t j;

    COUNT(tctx->stat_calls++);
    COUNT(tctx->stat_m_total+=ctx->m);

    if (buflen < ctx->m)
        return 0;

    while (pos <= (buflen - ctx->m)) {
        j = ctx->m - 1;
        d = ~0;

        do {
            uint16_t h = B2G_CUDA_HASH16(u8_tolower(buf[pos + j - 1]),u8_tolower(buf[pos + j]));
            d = ((d << 1) & ctx->B2G[h]);
            j = j - 1;
        } while (d != 0 && j != 0);

        /* (partial) match, move on to verification */
        if (d != 0) {
            COUNT(tctx->stat_d0++);
            //printf("output at pos %" PRIu32 ": ", pos); prt(buf + pos, ctx->m); printf("\n");

            /* get our patterns from the hash */
            uint16_t h = B2G_CUDA_HASH16(u8_tolower(buf[pos + ctx->m - 2]),u8_tolower(buf[pos + ctx->m - 1]));

            if (ctx->bloom[h] != NULL) {
                COUNT(tctx->stat_pminlen_calls++);
                COUNT(tctx->stat_pminlen_total+=ctx->pminlen[h]);

                if ((buflen - pos) < ctx->pminlen[h]) {
                    goto skip_loop;
                } else {
                    COUNT(tctx->stat_bloom_calls++);

                    if (BloomFilterTest(ctx->bloom[h], buf+pos, ctx->pminlen[h]) == 0) {
                        COUNT(tctx->stat_bloom_hits++);

                        //printf("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "\n", ctx->bloom[h], buflen, pos, ctx->pminlen[h]);
                        goto skip_loop;
                    }
                }
            }

            B2gCudaHashItem *hi = ctx->hash[h], *thi;
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                COUNT(tctx->stat_d0_hashloop++);
                B2gCudaPattern *p = ctx->parray[thi->idx];

                if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp_lowercase(p->ci, buf+pos, p->len) == 0) {
                        COUNT(tctx->stat_loop_match++);

                        matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    } else {
                        COUNT(tctx->stat_loop_no_match++);
                    }
                } else {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp(p->cs, buf+pos, p->len) == 0) {
                        COUNT(tctx->stat_loop_match++);

                        matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    } else {
                        COUNT(tctx->stat_loop_no_match++);
                    }
                }
            }
skip_loop:
            //pos = pos + ctx->s0;
            pos = pos + 1;
        } else {
            COUNT(tctx->stat_num_shift++);
            COUNT(tctx->stat_total_shift += (j + 1));

            pos = pos + j + 1;
        }
    }

    //printf("Total matches %" PRIu32 "\n", matches);
    return matches;
}

#ifdef B2G_CUDA_SEARCH2
uint32_t B2gCudaSearch2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                        PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen)
{
    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    B2gCudaPattern *p;
    B2gCudaHashItem *thi, *hi;

    if (buflen < 2)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        uint8_t h8 = u8_tolower(*buf);
        hi = &ctx->hash1[h8];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                    if (h8 == p->ci[0]) {
                        cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    }
                } else {
                    if (*buf == p->cs[0]) {
                        cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    }
                }
            }
        }

        /* save one conversion by reusing h8 */
        uint16_t h16 = B2G_HASH16(h8, u8_tolower(*(buf+1)));
        hi = ctx->hash2[h16];

        for (thi = hi; thi != NULL; thi = thi->nxt) {
            p = ctx->parray[thi->idx];

            if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                if (h8 == p->ci[0] && u8_tolower(*(buf+1)) == p->ci[1]) {
                    //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
//                    for (em = p->em; em; em = em->next) {
                        if (MpmVerifyMatch(mpm_thread_ctx, pmq, p->id))
                            cnt++;
//                    }
                }
            } else {
                if (*buf == p->cs[0] && *(buf+1) == p->cs[1]) {
                    //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
//                    for (em = p->em; em; em = em->next) {
                        if (MpmVerifyMatch(mpm_thread_ctx, pmq, p->id))
                            cnt++;
//                    }
                }
            }
        }
        buf += 1;
    }

    //printf("B2gSearch2: after 2byte cnt %" PRIu32 "\n", cnt);
    if (ctx->pat_x_cnt > 0) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBSearch(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B2gSearch1: after 2+byte cnt %" PRIu32 "\n", cnt);
    }
    return cnt;
}
#endif

uint32_t B2gCudaSearch1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx,
                        PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen)
{
    SCEnter();

    B2gCudaCtx *ctx = (B2gCudaCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    B2gCudaPattern *p;
    B2gCudaHashItem *thi, *hi;

    if (buflen == 0)
        SCReturnUInt(0);

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        uint8_t h = u8_tolower(*buf);
        hi = &ctx->hash1[h];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->len != 1)
                    continue;

                if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                    if (u8_tolower(*buf) == p->ci[0]) {
                        cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    }
                } else {
                    if (*buf == p->cs[0]) {
                        cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                    }
                }
            }
        }
        buf += 1;
    }

    //printf("B2gSearch1: after 1byte cnt %" PRIu32 "\n", cnt);
#ifdef B2G_CUDA_SEARCH2
    if (ctx->pat_2_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBSearch2(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B2gSearch1: after 2+byte cnt %" PRIu32 "\n", cnt);
    } else
#endif
    if (ctx->pat_x_cnt) {
        cnt += ctx->MBSearch(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
    }
    SCReturnUInt(cnt);
}

/*********************Cuda_Specific_Mgmt_Code_Starts_Here**********************/

/*
 * \brief This data holds all resources needed by a cuda stream such as
 *        device pointers, host pointers, CUmodule and Kernel function. These
 *        are only valid within the context of the associated B2gCudaMpmThreadCtxData.
 */
typedef struct B2gCudaMpmStreamData_ {
    /* Stream used for asynchronous memcopy in the Cuda MPM dispatcher.
     * This is != NULL if the memory is allocated page-locked, i.e. profile->page_locked
     * is set. Only streams != NULL are used for async. processing. */
    CUstream cuda_stream;

    /* Flag that indicates if there is some asynchronous operation in progress */
    uint8_t cuda_async;

    /* The CUmodule for this stream and corresponding handle. We need to load the
     * cuda module for every stream to avoid rebinding the kernel configuration
     * with every kernel call. */
    int b2g_cuda_cumodule_handle;
    CUmodule b2g_cuda_cumodule;

    /* the search kernel */
    CUfunction b2g_cuda_search_kernel;

    /* the cuda_search_kernel argument offsets */
    uint8_t b2g_cuda_search_kernel_arg0_offset;
    uint8_t b2g_cuda_search_kernel_arg1_offset;
    uint8_t b2g_cuda_search_kernel_arg2_offset;
    uint8_t b2g_cuda_search_kernel_arg3_offset;
    uint8_t b2g_cuda_search_kernel_arg4_offset;
    uint8_t b2g_cuda_search_kernel_arg5_offset;
    uint8_t b2g_cuda_search_kernel_arg_total;

    /* the results buffer to hold the match offsets for the packets */
    uint16_t *results_buffer;
    /* gpu buffer corresponding to the above buffer */
    CUdeviceptr cuda_results_buffer;

    /* gpu buffer corresponding to SCCudaPBPacketsBuffer->packets_buffer */
    CUdeviceptr cuda_packets_buffer;
    /* gpu buffer corresponding to SCCudaPBPacketsBuffer->packets_offset_buffer */
    CUdeviceptr cuda_packets_offset_buffer;
    /* gpu buffer corresponding to SCCudaPBPacketsBuffer->packets_payload_offset_buffer */
    CUdeviceptr cuda_packets_payload_offset_buffer;
    /* gpu buffer corresponding to the global symbol g_u8_lowercasetable
     * XXX Remove this.  Store it as a constant buffer inside the kernel*/
    CUdeviceptr cuda_g_u8_lowercasetable;
} B2gCudaMpmStreamData;

/*
 * \brief Cuda specific data for the MPM's thread context.
 */
typedef struct B2gCudaMpmThreadCtxData_ {
    int b2g_cuda_module_handle;
    CUcontext b2g_cuda_context;

    /* Data of the cuda streams of this context */
    B2gCudaMpmStreamData *stream_data;
    uint8_t no_of_streams;
    /* Data store for packet buffers that are currently processed */
    Tmq *tmq_streamq;
} B2gCudaMpmThreadCtxData;

/*
 * \brief Initialize data for the cuda streams.
 *
 * \param tctx    The thread context data of the Cuda MPM.
 * \param profile The cuda profile used by the MPM.
 *
 * \retval 0 on succes, -1 on failure.
 */
static int B2gCudaMpmStreamDataInit(B2gCudaMpmThreadCtxData *tctx, MpmCudaConf *profile)
{
    SCCudaHlModuleData *module_data = NULL;
    B2gCudaMpmStreamData *sd = NULL;
    uint8_t i = 0;

    module_data = SCCudaHlGetModuleData(tctx->b2g_cuda_module_handle);
    if (module_data == NULL) {
        SCLogError(SC_ERR_CUDA_HANDLER_ERROR,"No Cuda module data");
        goto error;
    }

    SCLogDebug("Initializing data for %"PRIu16" cuda streams", tctx->no_of_streams);
    for (i = 0; i < tctx->no_of_streams; ++i) {
        sd = &tctx->stream_data[i];

        /* Init cuda stream */
        if (profile->page_locked) {
            if (SCCudaStreamCreate(&sd->cuda_stream, 0) == -1) {
                SCLogError(SC_ERR_CUDA_ERROR, "Error creating Cuda stream.");
                exit(EXIT_FAILURE);
            }
        } else {
            SCLogDebug("Disabled asynchronous cuda processing");
            sd->cuda_stream = NULL;
        }

        /* Load the CUmodule */
        sd->b2g_cuda_cumodule_handle = SCCudaHlGetCudaModule(&sd->b2g_cuda_cumodule,
                                                             "util-mpm-b2g-cuda-kernel",
                                                             module_data->handle);
        if (sd->b2g_cuda_cumodule_handle == -1) {
            SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda module");
            goto error;
        }

        /* Get kernel from module */
        if (SCCudaModuleGetFunction(&sd->b2g_cuda_search_kernel,
                                    sd->b2g_cuda_cumodule,
                                    B2G_CUDA_SEARCHFUNC_NAME) == -1) {
            SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda function");
            goto error;
        }

        /* Configure kernel execution */
        if (SCCudaFuncSetBlockShape(sd->b2g_cuda_search_kernel, 32, 1, 1) == -1) {
            SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error setting function block shape");
            goto error;
        }

#define ALIGN_UP(offset, alignment) do { \
                (offset) = ((offset) + (alignment) - 1) & ~((alignment) - 1); \
        } while (0)

        int offset = 0;

        ALIGN_UP(offset, __alignof(void *));
        sd->b2g_cuda_search_kernel_arg0_offset = offset;
        offset += sizeof(void *);

        ALIGN_UP(offset, __alignof(void *));
        sd->b2g_cuda_search_kernel_arg1_offset = offset;
        offset += sizeof(void *);

        ALIGN_UP(offset, __alignof(void *));
        sd->b2g_cuda_search_kernel_arg2_offset = offset;
        offset += sizeof(void *);

        ALIGN_UP(offset, __alignof(void *));
        sd->b2g_cuda_search_kernel_arg3_offset = offset;
        offset += sizeof(void *);

        ALIGN_UP(offset, __alignof(uint16_t));
        sd->b2g_cuda_search_kernel_arg4_offset = offset;
        offset += sizeof(void *);

        ALIGN_UP(offset, __alignof(void *));
        sd->b2g_cuda_search_kernel_arg5_offset = offset;
        offset += sizeof(void *);

        tctx->stream_data[i].b2g_cuda_search_kernel_arg_total = offset;

        /* buffer to hold the b2g cuda mpm match results for 4000 packets.  The
         * extra 2 bytes(the extra + 1 ) is to hold the no of
         * matches for the payload.  The remaining profile->packet_size_limit
         * positions in the buffer is to hold the match offsets */
        if (profile->page_locked) {
            if (SCCudaMemHostAlloc((void**)&sd->results_buffer,
                                   sizeof(uint16_t) * (profile->packet_size_limit + 1) *
                                   profile->packet_buffer_limit,
                                   CU_MEMHOSTALLOC_PORTABLE) == -1){
                SCLogError(SC_ERR_CUDA_ERROR, "Error allocating page-locked memory\n");
                exit(EXIT_FAILURE);
            }
        } else {
            sd->results_buffer = SCMalloc(sizeof(uint16_t) *
                                          (profile->packet_size_limit + 1) *
                                          profile->packet_buffer_limit);
            if (sd->results_buffer == NULL) {
                SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
                exit(EXIT_FAILURE);
            }
        }

        if (SCCudaHlGetCudaDevicePtr(&sd->cuda_results_buffer,
                                    "MPM_B2G_RESULTS",
                                    sizeof(uint16_t) *
                                    (profile->packet_size_limit + 1) *
                                    profile->packet_buffer_limit,
                                    NULL, module_data->handle,
                                    sd->b2g_cuda_cumodule_handle) == -1) {
            goto error;
        }

        if (SCCudaHlGetCudaDevicePtr(&sd->cuda_g_u8_lowercasetable,
                                     "G_U8_LOWERCASETABLE", 256 * sizeof(char),
                                     g_u8_lowercasetable, module_data->handle,
                                     sd->b2g_cuda_cumodule_handle) == -1) {
            goto error;
        }

        if (SCCudaHlGetCudaDevicePtr(&sd->cuda_packets_buffer,
                                     "MPM_B2G_PACKETS_BUFFER",
                                     profile->packet_buffer_limit *
                                     (profile->packet_size_limit +
                                      sizeof(SCCudaPBPacketDataForGPUNonPayload)),
                                     NULL, module_data->handle,
                                     sd->b2g_cuda_cumodule_handle) == -1) {
            goto error;
        }

        if (SCCudaHlGetCudaDevicePtr(&sd->cuda_packets_offset_buffer,
                                     "MPM_B2G_PACKETS_BUFFER_OFFSETS",
                                     sizeof(uint32_t) * profile->packet_buffer_limit,
                                     NULL, module_data->handle,
                                     sd->b2g_cuda_cumodule_handle) == -1) {
            goto error;
        }

        if (SCCudaHlGetCudaDevicePtr(&sd->cuda_packets_payload_offset_buffer,
                                     "MPM_B2G_PACKETS_PAYLOAD_BUFFER_OFFSETS",
                                     sizeof(uint32_t) * profile->packet_buffer_limit,
                                     NULL, module_data->handle,
                                     sd->b2g_cuda_cumodule_handle) == -1) {
            goto error;
        }


        if (SCCudaParamSetv(sd->b2g_cuda_search_kernel,
                            sd->b2g_cuda_search_kernel_arg0_offset,
                            (void *)&sd->cuda_results_buffer,
                            sizeof(void *)) == -1) {
            goto error;
        }

        if (SCCudaParamSetv(sd->b2g_cuda_search_kernel,
                            sd->b2g_cuda_search_kernel_arg1_offset,
                            (void *)&sd->cuda_packets_buffer,
                            sizeof(void *)) == -1) {
            goto error;
        }

        if (SCCudaParamSetv(sd->b2g_cuda_search_kernel,
                            sd->b2g_cuda_search_kernel_arg2_offset,
                            (void *)&sd->cuda_packets_offset_buffer,
                            sizeof(void *)) == -1) {
            goto error;
        }

        if (SCCudaParamSetv(sd->b2g_cuda_search_kernel,
                sd->b2g_cuda_search_kernel_arg3_offset,
                            (void *)&sd->cuda_packets_payload_offset_buffer,
                            sizeof(void *)) == -1) {
            goto error;
        }

        if (SCCudaParamSetv(sd->b2g_cuda_search_kernel,
                            sd->b2g_cuda_search_kernel_arg5_offset,
                            (void *)&sd->cuda_g_u8_lowercasetable,
                            sizeof(void *)) == -1) {
            goto error;
        }

        if (SCCudaParamSetSize(sd->b2g_cuda_search_kernel,
                               sd->b2g_cuda_search_kernel_arg_total) == -1) {
            goto error;
        }
    }

    return 0;

  error:
    return -1;
}

/*
 * \brief DeInitialize data for the cuda streams.
 *
 * \param tctx    The thread context data of the Cuda MPM.
 * \param profile The cuda profile used by the MPM.
 *
 * \retval 0 on succes, -1 on failure
 */
static int B2gCudaMpmStreamDataDeInit(B2gCudaMpmThreadCtxData *tctx, MpmCudaConf *profile)
{
    B2gCudaMpmStreamData *sd = NULL;
    uint8_t i = 0;

    if (tctx == NULL || profile == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Arguments cannot be NULL");
        goto error;
    }

    SCLogDebug("DeInitializing data for %"PRIu16" cuda streams", tctx->no_of_streams);
    for (i = 0; i < tctx->no_of_streams; ++i) {
        sd = &tctx->stream_data[i];

        if (sd->cuda_stream != NULL) {
            if (SCCudaStreamDestroy(sd->cuda_stream) == -1) {
                SCLogError(SC_ERR_CUDA_ERROR, "Error deallocating Cuda stream ");
                goto error;
            }
        }
        if (profile->page_locked) {
            if (SCCudaMemFreeHost(sd->results_buffer) == -1) {
                SCLogError(SC_ERR_CUDA_ERROR, "Error deallocating pagelocked memory: "
                                              "results_buffer\n");
                goto error;
            }
        } else {
            SCFree(sd->results_buffer);
        }
        SCCudaHlFreeCudaDevicePtr("MPM_B2G_RESULTS",
                                  tctx->b2g_cuda_module_handle,
                                  sd->b2g_cuda_cumodule_handle);
        SCCudaHlFreeCudaDevicePtr("MPM_B2G_PACKETS_BUFFER",
                                  tctx->b2g_cuda_module_handle,
                                  sd->b2g_cuda_cumodule_handle);
        SCCudaHlFreeCudaDevicePtr("MPM_B2G_PACKETS_BUFFER_OFFSETS",
                                  tctx->b2g_cuda_module_handle,
                                  sd->b2g_cuda_cumodule_handle);
        SCCudaHlFreeCudaDevicePtr("MPM_B2G_PACKETS_PAYLOAD_BUFFER_OFFSETS",
                                  tctx->b2g_cuda_module_handle,
                                  sd->b2g_cuda_cumodule_handle);
        SCCudaHlFreeCudaDevicePtr("G_U8_LOWERCASETABLE",
                                  tctx->b2g_cuda_module_handle,
                                  sd->b2g_cuda_cumodule_handle);
    }

    SCFree(tctx->stream_data);

    return 0;

 error:
    return -1;
}

/**
 * \brief The Cuda MPM B2G module's thread init function.
 *
 * \param tv       Pointer to the ThreadVars which has invoked this function.
 * \param initdata Pointer to some user sent data.
 * \param data     Pointer to a pointer which can be used to send data to the
 *                 dispatcher thread.
 *
 * \retval TM_ECODE_OK Always.
 */
TmEcode B2gCudaMpmDispThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    MpmCudaConf *profile = NULL;
    SCCudaHlModuleData *module_data = (SCCudaHlModuleData *)initdata;

    if (PatternMatchDefaultMatcher() != MPM_B2G_CUDA) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "b2g cuda mpm sees mpm that is "
                   "not b2g_cuda");
        exit(EXIT_FAILURE);
    }

    if (SCCudaCtxPushCurrent(module_data->cuda_context) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error pushing cuda context");
    }

    B2gCudaMpmThreadCtxData *tctx = SCThreadMalloc(tv, sizeof(B2gCudaMpmThreadCtxData));
    if (unlikely(tctx == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memset(tctx, 0, sizeof(B2gCudaMpmThreadCtxData));

    tctx->b2g_cuda_module_handle = module_data->handle;

    /* Check configuration if streams and async operations can be used.
     * If (CC == 1.0 || page_locked is disabled) then one stream is used,
     * else more streams are used. When using the stream for async processing
     * please check if memory has been allocated page-locked. */
    profile = SCCudaHlGetProfile("mpm");

    SCCudaDevices *devices = SCCudaGetDeviceList();
    if (devices == NULL) {
        SCLogError(SC_ERR_CUDA_ERROR, "CUDA environment not initialized.  "
                   "Please initialized the CUDA environment by calling "
                   "SCCudaInitCudaEnvironment() before making any calls "
                   "to the CUDA API.");
        goto error;
    }
    if (profile->device_id >= devices->count) {
        SCLogError(SC_ERR_INVALID_YAML_CONF_ENTRY, "Cuda device does not exist.");
        goto error;
    }
    tctx->no_of_streams = profile->cuda_streams;

    if (!devices->devices[profile->device_id]->attr_gpu_overlap) {
        SCLogInfo("Cuda device does not support gpu overlap. Falling back to 1 stream.");
        tctx->no_of_streams = 1;
    }
    if (!profile->page_locked) {
        SCLogInfo("In order to use asynchronous operations you need to enable "
                  "page-locked memory in suricata.yaml.");
        tctx->no_of_streams = 1;
    }

    /* Initialize resources for the streams */
    tctx->stream_data = SCMalloc(tctx->no_of_streams * sizeof(B2gCudaMpmStreamData));
    if (tctx->stream_data == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory.");
        exit(EXIT_FAILURE);
    }
    memset(tctx->stream_data, 0, tctx->no_of_streams * sizeof(B2gCudaMpmStreamData));

    if (SCCudaHlGetCudaContext(&tctx->b2g_cuda_context, "mpm", module_data->handle) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda context");
        goto error;
    }

    /* Initialize stream data */
    if (B2gCudaMpmStreamDataInit(tctx, profile) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error initializing Cuda device data.");
        goto error;
    }

    /* Setup queue to hold packet buffers for stream processing */
    char *streamq_name = "b2g_cuda_mpm_streamq";
    tctx->tmq_streamq = TmqGetQueueByName(streamq_name);
    if (tctx->tmq_streamq == NULL) {
        tctx->tmq_streamq = TmqCreateQueue(streamq_name);
        if (tctx->tmq_streamq == NULL) {
            goto error;
        }
    }
    tctx->tmq_streamq->q_type = 1;
    tctx->tmq_streamq->reader_cnt++;
    tctx->tmq_streamq->writer_cnt++;

    *data = tctx;

    return TM_ECODE_OK;

 error:
    return TM_ECODE_FAILED;
}

/**
 * \brief The Cuda MPM B2G module's thread de-init function.
 *
 * \param tv   Pointer to the ThreadVars which has invoked this function.
 * \param data Pointer to the slot data if anything had been attached in
 *             the thread init function.
 *
 * \retval TM_ECODE_OK Always.
 */
TmEcode B2gCudaMpmDispThreadDeInit(ThreadVars *tv, void *data)
{
    B2gCudaMpmThreadCtxData *tctx = data;
    MpmCudaConf *profile = NULL;

    if (tctx == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Invalid arguments.  data NULL\n");
        return TM_ECODE_OK;
    }

    if (PatternMatchDefaultMatcher() != MPM_B2G_CUDA)
        return TM_ECODE_OK;

    CUcontext dummy_context;
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(tctx->b2g_cuda_module_handle);
    if (module_data == NULL) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "How did we even fail to get a "
                   "module_data if we are having a module_handle");
        goto error;
    }
    if (SCCudaHlGetCudaContext(&dummy_context, "mpm", tctx->b2g_cuda_module_handle) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda context for the "
                   "module %s", module_data->name);
        goto error;
    }
    SCCudaCtxPushCurrent(dummy_context);

    profile = SCCudaHlGetProfile("mpm");
    if (B2gCudaMpmStreamDataDeInit(tctx, profile) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error deallocating Cuda device data.");
        goto error;
    }

    SCFree(tctx);

    if (SCCudaCtxPopCurrent(NULL) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error popping cuda context");
    }

    return TM_ECODE_OK;

 error:
    return TM_ECODE_FAILED;
}


/*
 * \brief Process a packet buffer on the GPU.
 *
 * \param pb   Pointer to the packet buffer.
 * \param tctx Pointer to the thread context which contains the Cuda context, kernel module,
 *             streams, CPU+GPU memory etc.
 * \param s    ID of the stream to be used.
 *
 * \retval     0 on succes, -1 on failure
 */
static int B2gCudaMpmProcessBuffer(SCCudaPBPacketsBuffer *pb, B2gCudaMpmThreadCtxData *tctx, uint16_t s)
{
    B2gCudaMpmStreamData *sd = NULL;

    if (pb == NULL || tctx == NULL || s >= tctx->no_of_streams) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Either pb == NULL || tctx == NULL"
                   "or the CUstream does not exist");
        goto error;
    }

    SCLogDebug("Process packet buffer %p in stream %"PRIu16, pb, s);

    sd = &tctx->stream_data[s];

    if (sd->cuda_stream != NULL) {
        SCLogDebug("B2g Cuda: Asynchronous processing enabled.");
        sd->cuda_async = 1;
    }

    /* H->D */
    if (sd->cuda_async) {
        if (SCCudaMemcpyHtoDAsync(sd->cuda_packets_buffer,
                                  pb->packets_buffer,
                                  pb->packets_buffer_len,
                                  sd->cuda_stream) == -1) {
            goto error;
        }

        if (SCCudaMemcpyHtoDAsync(sd->cuda_packets_offset_buffer,
                                  pb->packets_offset_buffer,
                                  sizeof(uint32_t) * pb->nop_in_buffer,
                                  sd->cuda_stream) == -1) {
            goto error;
        }

        if (SCCudaMemcpyHtoDAsync(sd->cuda_packets_payload_offset_buffer,
                                  pb->packets_payload_offset_buffer,
                                  sizeof(uint32_t) * pb->nop_in_buffer,
                                  sd->cuda_stream) == -1) {
            goto error;
        }
    } else {
        if (SCCudaMemcpyHtoD(sd->cuda_packets_buffer,
                             pb->packets_buffer,
                             pb->packets_buffer_len) == -1) {
            goto error;
        }

        if (SCCudaMemcpyHtoD(sd->cuda_packets_offset_buffer,
                             pb->packets_offset_buffer,
                             sizeof(uint32_t) * pb->nop_in_buffer) == -1) {
            goto error;
        }

        if (SCCudaMemcpyHtoD(sd->cuda_packets_payload_offset_buffer,
                             pb->packets_payload_offset_buffer,
                             sizeof(uint32_t) * pb->nop_in_buffer) == -1) {
            goto error;
        }
    }

    if (SCCudaParamSeti(sd->b2g_cuda_search_kernel,
                        sd->b2g_cuda_search_kernel_arg4_offset,
                        pb->nop_in_buffer) == -1) {
        goto error;
    }

    /* Kernel:
     * the no of threads per block has already been set to 32
     * \todo if we are very sure we are allocating a multiple of block_size
     * buffer_threshold, then we can remove this + 1 here below */
    int no_of_cuda_blocks = (pb->nop_in_buffer / 32) + 1;
    if (sd->cuda_async) {
        if (SCCudaLaunchGridAsync(sd->b2g_cuda_search_kernel,
                                  no_of_cuda_blocks,
                                  1,
                                  sd->cuda_stream) == -1) {
            goto error;
        }
    } else {
        if (SCCudaLaunchGrid(sd->b2g_cuda_search_kernel,
                             no_of_cuda_blocks,
                             1) == -1) {
            goto error;
        }
    }

    /* D->H */
    if (sd->cuda_async) {
        if (SCCudaMemcpyDtoHAsync(sd->results_buffer,
                                  sd->cuda_results_buffer,
                                  sizeof(uint16_t) *
                                   (pb->nop_in_buffer + pb->packets_total_payload_len),
                                  sd->cuda_stream) == -1) {
            goto error;
        }
    } else {
        if (SCCudaMemcpyDtoH(sd->results_buffer,
                             sd->cuda_results_buffer,
                             sizeof(uint16_t) *
                              (pb->nop_in_buffer + pb->packets_total_payload_len)) == -1) {
            goto error;
        }
    }

    return 0;

 error:
    SCCudaCtxSynchronize();
    sd->cuda_async = 0;

    return -1;
}

/**
 * \brief The dispatcher function for the cuda mpm.  Takes a packet, feeds
 *        it to the gpu and informs the calling client when it has the
 *        results ready.
 *
 * \param tv   We don't need this.
 * \param incoming_buffer Pointer to the Packet which contains all the relevant data,
 *             like the bufffer, buflen, the contexts.
 * \param data Pointer to the slot data if anything had been attached in
 *             the thread init function.
 * \param buffer_dq Pointer to a data queue that can contain additional packet buffers
 *             that should be processed in other CUstreams (if enabled). The dispatcher
 *             function will dequeue all buffers that have been processed. The queue will
 *             not be changed if CUstreams are disabled.
 * \param post_pq We don't need this.
 *
 * \retval TM_ECODE_OK Always.
 */
TmEcode B2gCudaMpmDispatcher(ThreadVars *tv, Packet *incoming_buffer,
                             void *data, PacketQueue *buffer_dq, PacketQueue *post_pq)
{
    SCCudaPBPacketsBuffer *pb = (SCCudaPBPacketsBuffer *)incoming_buffer;
    B2gCudaMpmThreadCtxData *tctx = data;

    SCDQDataQueue *in_dq = (SCDQDataQueue *)buffer_dq;
    SCDQDataQueue *out_dq = &data_queues[tctx->tmq_streamq->id];
    SCDQGenericQData *q_ptr = NULL;
    SCCudaPBPacketsBuffer *pb_in_queue = NULL;
    uint8_t curr_stream = 0;

    uint32_t i = 0;

    SCLogDebug("Running the B2g CUDA mpm dispatcher");

    if (pb == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Invalid argument.  pb is NULL!!");
        return TM_ECODE_OK;
    }

    /* Start processing the incoming_buffer */
    if (B2gCudaMpmProcessBuffer(pb, tctx, curr_stream) == -1) {
        goto error;
    }

    /* Check if there are additional buffers in in_dq */
    while (in_dq != NULL && ++curr_stream < tctx->no_of_streams) {
        SCMutexLock(&in_dq->mutex_q);
        pb_in_queue = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(in_dq);
        SCMutexUnlock(&in_dq->mutex_q);
        if (pb_in_queue == NULL) {
            break;
        }

        /* Add buffer to out_dq and start processing */
        SCDQDataEnqueue(out_dq, (SCDQGenericQData *)pb_in_queue);
        if (B2gCudaMpmProcessBuffer(pb_in_queue, tctx, curr_stream) == -1) {
            goto error;
        }
    }

    /* Sync first packet buffer */
    curr_stream = 0;
    if (tctx->stream_data[curr_stream].cuda_async) {
        SCLogDebug("Synchronize PB %p in Cuda stream %"PRIu16, pb, curr_stream);
        if (SCCudaStreamSynchronize(tctx->stream_data[curr_stream].cuda_stream) == -1) {
            SCLogError(SC_ERR_CUDA_ERROR, "Failed to synchronize Cuda stream");
            goto error;
        }
        tctx->stream_data[curr_stream].cuda_async = 0;
    }
    i = 0;
    for (i = 0; i < pb->nop_in_buffer; i++) {
        memcpy(pb->packets_address_buffer[i]->mpm_offsets,
               (tctx->stream_data[curr_stream].results_buffer + i +
                pb->packets_payload_offset_buffer[i]),
               (pb->packets_address_buffer[i]->payload_len + 1) * sizeof(uint16_t));
        SCMutexLock(&pb->packets_address_buffer[i]->cuda_mutex);
        pb->packets_address_buffer[i]->cuda_done = 1;
        SCMutexUnlock(&pb->packets_address_buffer[i]->cuda_mutex);
        SCCondSignal(&pb->packets_address_buffer[i]->cuda_cond);
    }

    /* Sync all other buffers in out_dq (if any) */
    q_ptr = out_dq->bot;
    while (q_ptr != NULL && ++curr_stream < tctx->no_of_streams) {
        pb_in_queue = (SCCudaPBPacketsBuffer *)q_ptr;

        if (tctx->stream_data[curr_stream].cuda_async) {
            SCLogDebug("Synchronize PB %p in Cuda stream %"PRIu16, pb_in_queue, curr_stream);
            if (SCCudaStreamSynchronize(tctx->stream_data[curr_stream].cuda_stream) == -1) {
                SCLogError(SC_ERR_CUDA_ERROR, "Failed to synchronize Cuda stream");
                goto error;
            }
            tctx->stream_data[curr_stream].cuda_async = 0;
        }
        i = 0;
        for (i = 0; i < pb_in_queue->nop_in_buffer; i++) {
            memcpy(pb_in_queue->packets_address_buffer[i]->mpm_offsets,
                   (tctx->stream_data[curr_stream].results_buffer + i +
                    pb_in_queue->packets_payload_offset_buffer[i]),
                   (pb_in_queue->packets_address_buffer[i]->payload_len + 1) * sizeof(uint16_t));
            SCMutexLock(&pb_in_queue->packets_address_buffer[i]->cuda_mutex);
            pb_in_queue->packets_address_buffer[i]->cuda_done = 1;
            SCMutexUnlock(&pb_in_queue->packets_address_buffer[i]->cuda_mutex);
            SCCondSignal(&pb_in_queue->packets_address_buffer[i]->cuda_cond);
        }

        q_ptr = q_ptr->prev;
    }

    SCLogDebug("B2g Cuda mpm dispatcher returning");

    return TM_ECODE_OK;

 error:
    if (SCCudaCtxSynchronize() == -1) {
        SCLogError(SC_ERR_CUDA_ERROR, "Failed to synchronize context.");
    }

    curr_stream = 0;
    tctx->stream_data[curr_stream].cuda_async = 0;
    for (i = 0; i < pb->nop_in_buffer; i++) {
        SCMutexLock(&pb->packets_address_buffer[i]->cuda_mutex);
        pb->packets_address_buffer[i]->cuda_done = 1;
        SCMutexUnlock(&pb->packets_address_buffer[i]->cuda_mutex);
        SCCondSignal(&pb->packets_address_buffer[i]->cuda_cond);
    }

    q_ptr = out_dq->bot;
    while (q_ptr != NULL && ++curr_stream < tctx->no_of_streams) {
        pb_in_queue = (SCCudaPBPacketsBuffer *)q_ptr;
        tctx->stream_data[curr_stream].cuda_async = 0;

        for (i = 0; i < pb_in_queue->nop_in_buffer; i++) {
            SCMutexLock(&pb_in_queue->packets_address_buffer[i]->cuda_mutex);
            pb_in_queue->packets_address_buffer[i]->cuda_done = 1;
            SCMutexUnlock(&pb_in_queue->packets_address_buffer[i]->cuda_mutex);
            SCCondSignal(&pb_in_queue->packets_address_buffer[i]->cuda_cond);
        }

        q_ptr = q_ptr->prev;
    }

    SCLogError(SC_ERR_B2G_CUDA_ERROR, "B2g Cuda mpm dispatcher returning with error");

    return TM_ECODE_OK;
}

/**
 * \brief The post processing of cuda mpm b2g results for a packet
 *        is done here.  Will be used by the detection thread.  We basically
 *        obtain the match offsets from the cuda mpm search and carry out
 *        further matches on those offsets.  Also if the results are not
 *        read for a packet, we wait on the conditional, which will then
 *        be signalled by the cuda mpm dispatcher thread, once the results
 *        for the packet are ready.
 *
 * \param p              Pointer to the packet whose mpm cuda results are
 *                       to be further processed.
 * \param mpm_ctx        Pointer to the mpm context for this packet.
 * \param mpm_thread_ctx Pointer to the mpm thread context.
 * \param pmq            Pointer to the patter matcher queue.
 *
 * \retval matches Holds the no of matches.
 */
int B2gCudaResultsPostProcessing(Packet *p, MpmCtx *mpm_ctx,
                                 MpmThreadCtx *mpm_thread_ctx,
                                 PatternMatcherQueue *pmq)
{
    B2gCudaCtx *ctx = mpm_ctx->ctx;

    while (p->cuda_done == 0) {
        SCMutexLock(&p->cuda_mutex);
        if (p->cuda_done == 1) {
            SCMutexUnlock(&p->cuda_mutex);
            break;
        } else {
            SCCondWait(&p->cuda_cond, &p->cuda_mutex);
            SCMutexUnlock(&p->cuda_mutex);
        }
    }

    /* reset this flag for the packet */
    p->cuda_done = 0;

    uint16_t *no_of_matches = p->mpm_offsets;
    uint16_t *host_offsets = p->mpm_offsets + 1;
    int i = 0, h = 0;
    uint8_t *buf = p->payload;
    uint16_t buflen = p->payload_len;
    int matches = 0;
    for (i = 0; i < no_of_matches[0]; i++) {
        h = B2G_CUDA_HASH16(u8_tolower(buf[host_offsets[i] + ctx->m - 2]),
                            u8_tolower(buf[host_offsets[i] + ctx->m - 1]));

        if (ctx->bloom[h] != NULL) {
            COUNT(tctx->stat_pminlen_calls++);
            COUNT(tctx->stat_pminlen_total+=ctx->pminlen[h]);

            if ((buflen - host_offsets[i]) < ctx->pminlen[h]) {
                continue;
            } else {
                COUNT(tctx->stat_bloom_calls++);

                if (BloomFilterTest(ctx->bloom[h], buf + host_offsets[i], ctx->pminlen[h]) == 0) {
                    COUNT(tctx->stat_bloom_hits++);

                    continue;
                }
            }
        }

        B2gCudaHashItem *hi = ctx->hash[h], *thi;
        for (thi = hi; thi != NULL; thi = thi->nxt) {
            COUNT(tctx->stat_d0_hashloop++);
            B2gCudaPattern *p = ctx->parray[thi->idx];

            if (p->flags & MPM_PATTERN_FLAG_NOCASE) {
                if ((buflen - host_offsets[i]) < p->len) {
                    continue;
                }

                if (memcmp_lowercase(p->ci, buf + host_offsets[i], p->len) == 0) {
                    COUNT(tctx->stat_loop_match++);

                    matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                } else {
                    COUNT(tctx->stat_loop_no_match++);
                }
            } else {
                if (buflen - host_offsets[i] < p->len)
                    continue;

                if (memcmp(p->cs, buf +  host_offsets[i], p->len) == 0) {
                    COUNT(tctx->stat_loop_match++);

                    matches += MpmVerifyMatch(mpm_thread_ctx, pmq, p->id);
                } else {
                    COUNT(tctx->stat_loop_no_match++);
                }
            }
        }
    }

    return matches;
}

/**
 * \brief Registers the Cuda B2G MPM Module.
 */
void TmModuleCudaMpmB2gRegister(void)
{
    tmm_modules[TMM_CUDA_MPM_B2G].name = "Cuda_Mpm_B2g";
    tmm_modules[TMM_CUDA_MPM_B2G].ThreadInit = B2gCudaMpmDispThreadInit;
    tmm_modules[TMM_CUDA_MPM_B2G].Func = B2gCudaMpmDispatcher;
    tmm_modules[TMM_CUDA_MPM_B2G].ThreadExitPrintStats = NULL;
    tmm_modules[TMM_CUDA_MPM_B2G].ThreadDeinit = B2gCudaMpmDispThreadDeInit;
    tmm_modules[TMM_CUDA_MPM_B2G].RegisterTests = NULL;
}

/***************************Code_Specific_To_Mpm_B2g***************************/

void *CudaMpmB2gThreadsSlot1(void *td)
{
    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    SCCudaPBPacketsBuffer *data = NULL;
    B2gCudaMpmThreadCtxData *tctx = NULL;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    SCSetThreadName(tv->name);

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    SCLogDebug("%s starting", tv->name);

    if (s->SlotThreadInit != NULL) {
        void *slot_data = NULL;
        r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
        if (r != TM_ECODE_OK) {
            EngineKill();

            TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
            pthread_exit((void *) -1);
        }
        SC_ATOMIC_SET(s->slot_data, slot_data);
    }
    memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
    memset(&s->slot_post_pq, 0, sizeof(PacketQueue));

    tctx = (B2gCudaMpmThreadCtxData *)SC_ATOMIC_GET(s->slot_data);

    TmThreadsSetFlag(tv, THV_INIT_DONE);
    while(run) {
        TmThreadTestThreadUnPaused(tv);

        /* input data */
        data = (SCCudaPBPacketsBuffer *)TmqhInputSimpleOnQ(&data_queues[tv->inq->id]);

        if (data == NULL) {
            //printf("%s: TmThreadsSlot1: p == NULL\n", tv->name);
        } else {
            /* We pass the current packet buffer (1) to the dispatcher function. The data queue
             * is checked by the dispatcher thread if there is another packet buffer (2) ready.
             * If the MPM is configured to use multiple CUstreams, buffer (1) and buffer (2) are
             * processed in parallel using multiple streams; In this case
             * data_queues[tctx->tmq_streamq->id] will contain the results of packet buffer (2). */
            TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);
            r = SlotFunc(tv,
                              (Packet *)data,
                              (void *)tctx,
                              (PacketQueue *)&data_queues[tv->inq->id],
                              NULL);
            /* handle error */

            /* output the packet buffer (1) */
            TmqhOutputSimpleOnQ(&data_queues[tv->outq->id], (SCDQGenericQData *)data);

            /* output additional packet buffers (2) */
            while (data != NULL) {
                data = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[tctx->tmq_streamq->id]);
                if (data == NULL) {
                    break;
                }
                TmqhOutputSimpleOnQ(&data_queues[tv->outq->id], (SCDQGenericQData *)data);
            }
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            run = 0;
        }
    }

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    if (s->SlotThreadExitPrintStats != NULL) {
        s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
    }

    if (s->SlotThreadDeinit != NULL) {
        r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
        if (r != TM_ECODE_OK) {
            TmThreadsSetFlag(tv, THV_CLOSED);
            pthread_exit((void *) -1);
        }
    }

    SCLogDebug("%s ending", tv->name);
    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
}

int B2gCudaStartDispatcherThreadRC(const char *name)
{
    SCCudaHlModuleData *data = NULL;
    TmModule *tm_module = NULL;

    if (name == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Error invalid arguments.  "
                   "name NULL");
        return -1;
    }

    if (tv_CMB2_RC != NULL) {
        SCLogError(SC_ERR_TM_THREADS_ERROR, "We already have this thread "
                   "running from b2g-cuda");
        return 0;
    }

    data = SCCudaHlGetModuleData(SCCudaHlGetModuleHandle(name));
    if (data == NULL) {
        SCLogDebug("Module not registered.  To avail the benefits of this "
                   "registration facility, first register a module using "
                   "context using SCCudaHlRegisterModule(), after which you "
                   "can call this function");
        return -1;
    }

    /* create the threads */
    tv_CMB2_RC = TmThreadCreate("Cuda_Mpm_B2g_RC",
                                "cuda_batcher_mpm_outqueue", "simple",
                                "cuda_batcher_mpm_inqueue", "simple",
                                "custom", CudaMpmB2gThreadsSlot1, 0);
    if (tv_CMB2_RC == NULL) {
        SCLogError(SC_ERR_TM_THREADS_ERROR, "ERROR: TmThreadsCreate failed");
        exit(EXIT_FAILURE);
    }
    tv_CMB2_RC->type = TVT_PPT;
    tv_CMB2_RC->inq->q_type = 1;
    tv_CMB2_RC->outq->q_type = 1;

    tm_module = TmModuleGetByName("Cuda_Mpm_B2g");
    if (tm_module == NULL) {
        SCLogError(SC_ERR_TM_MODULES_ERROR,
                   "ERROR: TmModuleGetByName failed for Cuda_Mpm_B2g_RC");
        exit(EXIT_FAILURE);
    }
    TmSlotSetFuncAppend(tv_CMB2_RC, tm_module, data);

    if (TmThreadSpawn(tv_CMB2_RC) != TM_ECODE_OK) {
        SCLogError(SC_ERR_TM_THREADS_ERROR, "ERROR: TmThreadSpawn failed");
        exit(EXIT_FAILURE);
    }

    TmThreadContinue(tv_CMB2_RC);

    return 0;
}

/**
 * \brief Hacks for the tests.  While running the tests, we sometimes need to
 *        kill the threads to make them pop the cuda contexts.  We don't need
 *        these under normal running.
 */
void B2gCudaKillDispatcherThreadRC(void)
{
    if (tv_CMB2_RC == NULL)
        return;

    TmThreadKillThread(tv_CMB2_RC);
    TmThreadRemove(tv_CMB2_RC, tv_CMB2_RC->type);
    SCFree(tv_CMB2_RC);
    tv_CMB2_RC = NULL;

    return;
}

/*********************************Unittests************************************/

#ifdef UNITTESTS


static int B2gCudaTest01(void)
{
    Packet *p = NULL;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    B2gCudaCtx *ctx = NULL;
    int result = 0;
    int module_handle = SCCudaHlRegisterModule("B2G_CUDA_TEST");
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(module_handle);
    SCCudaPBPacketsBuffer *pb = NULL;

    /* get the cuda context and push it */
    CUcontext dummy_context;
    if (SCCudaHlGetCudaContext(&dummy_context, "mpm", module_handle) == -1) {
        SCLogError(SC_ERR_B2G_CUDA_ERROR, "Error getting a cuda context for the "
                   "module SC_RULES_CONTENT_B2G_CUDA");
    }
    SCCudaCtxPushCurrent(dummy_context);

    memset(&mpm_ctx, 0, sizeof(MpmCtx));
    B2gCudaInitCtx(&mpm_ctx, module_handle);
    /* pop the context before we make further calls to the mpm cuda dispatcher */
    SCCudaCtxPopCurrent(NULL);

    B2gCudaMpmThreadCtxData *tctx = NULL;
    B2gCudaMpmDispThreadInit(NULL, module_data, (void *)&tctx);

    ctx = mpm_ctx.ctx;

    if (tctx->b2g_cuda_context == 0)
        goto end;
    if (tctx->stream_data[0].b2g_cuda_cumodule == 0)
        goto end;
    if (tctx->stream_data[0].b2g_cuda_search_kernel == 0)
        goto end;

    if (B2gCudaAddPatternCS(&mpm_ctx, (uint8_t *)"one", 3, 0, 0, 1, 1, 0) == -1)
        goto end;

    if (B2gCudaPreparePatterns(&mpm_ctx) == -1)
        goto end;
    B2gCudaThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    result = 1;

    SCCudaPBSetProfile("mpm");
    pb = SCCudaPBAllocSCCudaPBPacketsBuffer();
    SCCudaPBPacketDataForGPU *curr_packet = (SCCudaPBPacketDataForGPU *)pb->packets_buffer;

    char *string = "tone_one_one_one";
    curr_packet->m = ctx->m;
    curr_packet->table = ctx->cuda_B2G;
    curr_packet->payload_len = strlen(string);
    memcpy(curr_packet->payload, string, strlen(string));

    pb->nop_in_buffer = 1;
    pb->packets_buffer_len = sizeof(SCCudaPBPacketDataForGPUNonPayload) + strlen(string);
    pb->packets_total_payload_len = strlen(string);
    pb->packets_offset_buffer[0] = 0;
    pb->packets_payload_offset_buffer[0] = 0;

    p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        goto end;
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    pb->packets_address_buffer[0] = p;
    p->payload_len = strlen(string);

    B2gCudaMpmDispatcher(NULL, (Packet *)pb, tctx, NULL, NULL);

    result &= (p->mpm_offsets[0] == 4);
    result &= (p->mpm_offsets[1] == 1);
    result &= (p->mpm_offsets[2] == 5);
    result &= (p->mpm_offsets[3] == 9);
    result &= (p->mpm_offsets[4] == 13);

 end:
    SCCudaPBDeAllocSCCudaPBPacketsBuffer(pb);
    B2gCudaMpmDispThreadDeInit(NULL, (void *)tctx);
    B2gCudaDestroyCtx(&mpm_ctx);
    B2gCudaThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    SCFree(p);
    return result;
}

static int B2gCudaTest02(void)
{
    uint8_t raw_eth[] = {
        0x00, 0x25, 0x00, 0x9e, 0xfa, 0xfe, 0x00, 0x02,
        0xcf, 0x74, 0xfe, 0xe1, 0x08, 0x00, 0x45, 0x00,
        0x01, 0xcc, 0xcb, 0x91, 0x00, 0x00, 0x34, 0x06,
        0xdf, 0xa8, 0xd1, 0x55, 0xe3, 0x67, 0xc0, 0xa8,
        0x64, 0x8c, 0x00, 0x50, 0xc0, 0xb7, 0xd1, 0x11,
        0xed, 0x63, 0x81, 0xa9, 0x9a, 0x05, 0x80, 0x18,
        0x00, 0x75, 0x0a, 0xdd, 0x00, 0x00, 0x01, 0x01,
        0x08, 0x0a, 0x09, 0x8a, 0x06, 0xd0, 0x12, 0x21,
        0x2a, 0x3b, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31,
        0x2e, 0x31, 0x20, 0x33, 0x30, 0x32, 0x20, 0x46,
        0x6f, 0x75, 0x6e, 0x64, 0x0d, 0x0a, 0x4c, 0x6f,
        0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x0d, 0x0a, 0x43,
        0x61, 0x63, 0x68, 0x65, 0x2d, 0x43, 0x6f, 0x6e,
        0x74, 0x72, 0x6f, 0x6c, 0x3a, 0x20, 0x70, 0x72,
        0x69, 0x76, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x43,
        0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54,
        0x79, 0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x20,
        0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d,
        0x55, 0x54, 0x46, 0x2d, 0x38, 0x0d, 0x0a, 0x44,
        0x61, 0x74, 0x65, 0x3a, 0x20, 0x4d, 0x6f, 0x6e,
        0x2c, 0x20, 0x31, 0x34, 0x20, 0x53, 0x65, 0x70,
        0x20, 0x32, 0x30, 0x30, 0x39, 0x20, 0x30, 0x38,
        0x3a, 0x34, 0x38, 0x3a, 0x33, 0x31, 0x20, 0x47,
        0x4d, 0x54, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
        0x65, 0x72, 0x3a, 0x20, 0x67, 0x77, 0x73, 0x0d,
        0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
        0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a,
        0x20, 0x32, 0x31, 0x38, 0x0d, 0x0a, 0x0d, 0x0a,
        0x3c, 0x48, 0x54, 0x4d, 0x4c, 0x3e, 0x3c, 0x48,
        0x45, 0x41, 0x44, 0x3e, 0x3c, 0x6d, 0x65, 0x74,
        0x61, 0x20, 0x68, 0x74, 0x74, 0x70, 0x2d, 0x65,
        0x71, 0x75, 0x69, 0x76, 0x3d, 0x22, 0x63, 0x6f,
        0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x74, 0x79,
        0x70, 0x65, 0x22, 0x20, 0x63, 0x6f, 0x6e, 0x74,
        0x65, 0x6e, 0x74, 0x3d, 0x22, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x63,
        0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x75,
        0x74, 0x66, 0x2d, 0x38, 0x22, 0x3e, 0x0a, 0x3c,
        0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x33, 0x30,
        0x32, 0x20, 0x4d, 0x6f, 0x76, 0x65, 0x64, 0x3c,
        0x2f, 0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x3c,
        0x2f, 0x48, 0x45, 0x41, 0x44, 0x3e, 0x3c, 0x42,
        0x4f, 0x44, 0x59, 0x3e, 0x0a, 0x3c, 0x48, 0x31,
        0x3e, 0x33, 0x30, 0x32, 0x20, 0x4d, 0x6f, 0x76,
        0x65, 0x64, 0x3c, 0x2f, 0x48, 0x31, 0x3e, 0x0a,
        0x54, 0x68, 0x65, 0x20, 0x64, 0x6f, 0x63, 0x75,
        0x6d, 0x65, 0x6e, 0x74, 0x20, 0x68, 0x61, 0x73,
        0x20, 0x6d, 0x6f, 0x76, 0x65, 0x64, 0x0a, 0x3c,
        0x41, 0x20, 0x48, 0x52, 0x45, 0x46, 0x3d, 0x22,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x22, 0x3e, 0x68,
        0x65, 0x72, 0x65, 0x3c, 0x2f, 0x41, 0x3e, 0x2e,
        0x0d, 0x0a, 0x3c, 0x2f, 0x42, 0x4f, 0x44, 0x59,
        0x3e, 0x3c, 0x2f, 0x48, 0x54, 0x4d, 0x4c, 0x3e,
        0x0d, 0x0a };

    int result = 0;
    const char *strings[10] = {
        "test_test_one",
        "test_two_test",
        "test_three_test",
        "test_four_test",
        "test_five_test",
        "test_six_test",
        "test_seven_test",
        "test_eight_test",
        "test_nine_test",
        "test_ten_test"};
    /* don't shoot me for hardcoding the results.  We will change this in
     * sometime, by running a separate mpm on the cpu, and then hold
     * the results in this temp buffer */
    int results[10][2] = { {0, 5},
                           {0, 9},
                           {0, 11},
                           {0, 10},
                           {0, 10},
                           {0, 9},
                           {0, 11},
                           {0, 11},
                           {0, 10},
                           {0, 9} };
    Packet *p[10];
    SCCudaPBThreadCtx *pb_tctx = NULL;

    DecodeThreadVars dtv;
    ThreadVars tv;
    DetectEngineCtx *de_ctx = NULL;

    SCCudaPBPacketsBuffer *pb = NULL;
    SCDQDataQueue *dq = NULL;

    char *inq_name = "cuda_batcher_mpm_inqueue";
    char *outq_name = "cuda_batcher_mpm_outqueue";

    Tmq *tmq_outq = NULL;
    Tmq *tmq_inq = NULL;

    uint32_t i = 0, j = 0;

    uint8_t no_of_pkts = 10;

    memset(&dtv, 0, sizeof(DecodeThreadVars));
    memset(&tv, 0, sizeof(ThreadVars));

    FlowInitConfig(FLOW_QUIET);

    memset(p, 0, sizeof(p));
    for (i = 0; i < no_of_pkts; i++) {
        p[i] = SCMalloc(SIZE_OF_PACKET);
        if (p[i] == NULL) {
            printf("error allocating memory\n");
            exit(EXIT_FAILURE);
        }
        memset(p[i], 0, SIZE_OF_PACKET);
        p[i]->pkt = (uint8_t *)(p[i] + 1);
        DecodeEthernet(&tv, &dtv, p[i], raw_eth, sizeof(raw_eth), NULL);
    }

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->mpm_matcher = MPM_B2G_CUDA;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                               "content:\"test\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    SigGroupBuild(de_ctx);

    SCCudaPBSetProfile("mpm");
    SCCudaPBSetUpQueuesAndBuffers();

    /* get the queues used by the batcher thread */
    tmq_inq = TmqGetQueueByName(inq_name);
    if (tmq_inq == NULL) {
        printf("tmq_inq NULL\n");
        goto end;
    }
    tmq_outq = TmqGetQueueByName(outq_name);
    if (tmq_outq == NULL) {
        printf("tmq_outq NULL\n");
        goto end;
    }

    result = 1;

    /* queue state before calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 10);

    SCCudaPBRunningTests(1);
    /* init the TM thread */
    SCCudaPBThreadInit(&tv, de_ctx, (void *)&pb_tctx);
    SCCudaPBSetBufferPacketThreshhold(no_of_pkts);

    /* queue state after calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 9);

    pb = pb_tctx->curr_pb;

    for (i = 0; i < no_of_pkts; i++) {
        p[i]->payload = (uint8_t *)strings[i];
        p[i]->payload_len = strlen(strings[i]);
        SCCudaPBBatchPackets(NULL, p[i], pb_tctx, NULL, NULL);
    }

    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 1);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 8);

    result &= (pb->nop_in_buffer == no_of_pkts);

    int module_handle = SCCudaHlRegisterModule("SC_RULES_CONTENT_B2G_CUDA");
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(module_handle);

    B2gCudaMpmThreadCtxData *b2g_tctx = NULL;
    B2gCudaMpmDispThreadInit(NULL, module_data, (void *)&b2g_tctx);

    if (b2g_tctx->b2g_cuda_context == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_cumodule == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_search_kernel == 0) {
        result = 0;
        goto end;
    }

    B2gCudaMpmDispatcher(NULL, (Packet *)pb, b2g_tctx, NULL, NULL);

    for (i = 0; i < no_of_pkts; i++) {
        for (j = 0; j < p[i]->mpm_offsets[0]; j++)
            result &= (results[i][j] == p[i]->mpm_offsets[j + 1]);
    }

 end:
    for (i = 0; i < no_of_pkts; i++) {
        SCFree(p[i]);
    }
    SCCudaPBCleanUpQueuesAndBuffers();
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    SCCudaPBThreadDeInit(NULL, (void *)pb_tctx);
    B2gCudaMpmDispThreadDeInit(NULL, (void *)b2g_tctx);

    return result;
}

static int B2gCudaTest03(void)
{
    uint8_t raw_eth[] = {
        0x00, 0x25, 0x00, 0x9e, 0xfa, 0xfe, 0x00, 0x02,
        0xcf, 0x74, 0xfe, 0xe1, 0x08, 0x00, 0x45, 0x00,
        0x01, 0xcc, 0xcb, 0x91, 0x00, 0x00, 0x34, 0x06,
        0xdf, 0xa8, 0xd1, 0x55, 0xe3, 0x67, 0xc0, 0xa8,
        0x64, 0x8c, 0x00, 0x50, 0xc0, 0xb7, 0xd1, 0x11,
        0xed, 0x63, 0x81, 0xa9, 0x9a, 0x05, 0x80, 0x18,
        0x00, 0x75, 0x0a, 0xdd, 0x00, 0x00, 0x01, 0x01,
        0x08, 0x0a, 0x09, 0x8a, 0x06, 0xd0, 0x12, 0x21,
        0x2a, 0x3b, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31,
        0x2e, 0x31, 0x20, 0x33, 0x30, 0x32, 0x20, 0x46,
        0x6f, 0x75, 0x6e, 0x64, 0x0d, 0x0a, 0x4c, 0x6f,
        0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x0d, 0x0a, 0x43,
        0x61, 0x63, 0x68, 0x65, 0x2d, 0x43, 0x6f, 0x6e,
        0x74, 0x72, 0x6f, 0x6c, 0x3a, 0x20, 0x70, 0x72,
        0x69, 0x76, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x43,
        0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54,
        0x79, 0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x20,
        0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d,
        0x55, 0x54, 0x46, 0x2d, 0x38, 0x0d, 0x0a, 0x44,
        0x61, 0x74, 0x65, 0x3a, 0x20, 0x4d, 0x6f, 0x6e,
        0x2c, 0x20, 0x31, 0x34, 0x20, 0x53, 0x65, 0x70,
        0x20, 0x32, 0x30, 0x30, 0x39, 0x20, 0x30, 0x38,
        0x3a, 0x34, 0x38, 0x3a, 0x33, 0x31, 0x20, 0x47,
        0x4d, 0x54, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
        0x65, 0x72, 0x3a, 0x20, 0x67, 0x77, 0x73, 0x0d,
        0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
        0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a,
        0x20, 0x32, 0x31, 0x38, 0x0d, 0x0a, 0x0d, 0x0a,
        0x3c, 0x48, 0x54, 0x4d, 0x4c, 0x3e, 0x3c, 0x48,
        0x45, 0x41, 0x44, 0x3e, 0x3c, 0x6d, 0x65, 0x74,
        0x61, 0x20, 0x68, 0x74, 0x74, 0x70, 0x2d, 0x65,
        0x71, 0x75, 0x69, 0x76, 0x3d, 0x22, 0x63, 0x6f,
        0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x74, 0x79,
        0x70, 0x65, 0x22, 0x20, 0x63, 0x6f, 0x6e, 0x74,
        0x65, 0x6e, 0x74, 0x3d, 0x22, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x63,
        0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x75,
        0x74, 0x66, 0x2d, 0x38, 0x22, 0x3e, 0x0a, 0x3c,
        0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x33, 0x30,
        0x32, 0x20, 0x4d, 0x6f, 0x76, 0x65, 0x64, 0x3c,
        0x2f, 0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x3c,
        0x2f, 0x48, 0x45, 0x41, 0x44, 0x3e, 0x3c, 0x42,
        0x4f, 0x44, 0x59, 0x3e, 0x0a, 0x3c, 0x48, 0x31,
        0x3e, 0x33, 0x30, 0x32, 0x20, 0x4d, 0x6f, 0x76,
        0x65, 0x64, 0x3c, 0x2f, 0x48, 0x31, 0x3e, 0x0a,
        0x54, 0x68, 0x65, 0x20, 0x64, 0x6f, 0x63, 0x75,
        0x6d, 0x65, 0x6e, 0x74, 0x20, 0x68, 0x61, 0x73,
        0x20, 0x6d, 0x6f, 0x76, 0x65, 0x64, 0x0a, 0x3c,
        0x41, 0x20, 0x48, 0x52, 0x45, 0x46, 0x3d, 0x22,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x22, 0x3e, 0x68,
        0x65, 0x72, 0x65, 0x3c, 0x2f, 0x41, 0x3e, 0x2e,
        0x0d, 0x0a, 0x3c, 0x2f, 0x42, 0x4f, 0x44, 0x59,
        0x3e, 0x3c, 0x2f, 0x48, 0x54, 0x4d, 0x4c, 0x3e,
        0x0d, 0x0a };

    int result = 0;
    const char *strings[10] = {
        "test_test_one",
        "test_two_test",
        "test_three_test",
        "test_four_test",
        "test_five_test",
        "test_six_test",
        "test_seven_test",
        "test_eight_test",
        "test_nine_test",
        "test_ten_test"};
    /* don't shoot me for hardcoding the results.  We will change this in
     * sometime, by having run a separate mpm on the cpu and then hold
     * the results in a temp buffer */
    Packet *p[10];
    SCCudaPBThreadCtx *pb_tctx = NULL;

    DecodeThreadVars dtv;
    ThreadVars tv;
    DetectEngineCtx *de_ctx = NULL;
    DetectEngineThreadCtx *det_ctx;
    ThreadVars de_tv;

    SCCudaPBPacketsBuffer *pb = NULL;
    SCDQDataQueue *dq = NULL;

    char *inq_name = "cuda_batcher_mpm_inqueue";
    char *outq_name = "cuda_batcher_mpm_outqueue";

    Tmq *tmq_outq = NULL;
    Tmq *tmq_inq = NULL;

    uint32_t i = 0, j = 0;

    uint8_t no_of_pkts = 10;

    Signature *sig = NULL;

    memset(&dtv, 0, sizeof(DecodeThreadVars));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&de_tv, 0, sizeof(ThreadVars));

    FlowInitConfig(FLOW_QUIET);
    for (i = 0; i < no_of_pkts; i++) {
        p[i] = SCMalloc(SIZE_OF_PACKET);
        if (p[i] == NULL) {
            printf("error allocating memory\n");
            exit(EXIT_FAILURE);
        }
        memset(p[i], 0, SIZE_OF_PACKET);
        p[i]->pkt = (uint8_t *)(p[i] + 1);
        DecodeEthernet(&tv, &dtv, p[i], raw_eth, sizeof(raw_eth), NULL);
    }

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G_CUDA;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                               "content:\"test\"; sid:0;)");
    if (de_ctx->sig_list == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = de_ctx->sig_list;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"one\"; sid:1;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"two\"; sid:2;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"three\"; sid:3;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"four\"; sid:4;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"five\"; sid:5;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"six\"; sid:6;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"seven\"; sid:7;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"eight\"; sid:8;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"nine\"; sid:9;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"ten\"; sid:10;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }

    /* build the signatures */
    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&de_tv, (void *)de_ctx, (void *)&det_ctx);

    SCCudaPBSetProfile("mpm");
    SCCudaPBSetUpQueuesAndBuffers();

    /* get the queues used by the batcher thread */
    tmq_inq = TmqGetQueueByName(inq_name);
    if (tmq_inq == NULL) {
        printf("tmq_inq NULL\n");
        goto end;
    }
    tmq_outq = TmqGetQueueByName(outq_name);
    if (tmq_outq == NULL) {
        printf("tmq_outq NULL\n");
        goto end;
    }

    result = 1;

    /* queue state before calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 10);

    SCCudaPBRunningTests(1);
    /* init the TM thread */
    SCCudaPBThreadInit(&tv, de_ctx, (void *)&pb_tctx);
    SCCudaPBSetBufferPacketThreshhold(no_of_pkts);

    /* queue state after calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 9);

    pb = pb_tctx->curr_pb;

    for (i = 0; i < no_of_pkts; i++) {
        p[i]->payload = (uint8_t *)strings[i];
        p[i]->payload_len = strlen(strings[i]);
        SCCudaPBBatchPackets(NULL, p[i], pb_tctx, NULL, NULL);
    }

    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 1);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 8);

    result &= (pb->nop_in_buffer == no_of_pkts);

    int module_handle = SCCudaHlRegisterModule("SC_RULES_CONTENT_B2G_CUDA");
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(module_handle);

    B2gCudaMpmThreadCtxData *b2g_tctx = NULL;
    B2gCudaMpmDispThreadInit(NULL, module_data, (void *)&b2g_tctx);

    if (b2g_tctx->b2g_cuda_context == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_cumodule == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_search_kernel == 0) {
        result = 0;
        goto end;
    }

    B2gCudaMpmDispatcher(NULL, (Packet *)pb, b2g_tctx, NULL, NULL);

    for (i = 0; i < 10; i++)
        SigMatchSignatures(&de_tv, de_ctx, det_ctx, p[i]);

    for (i = 0; i < 10; i++) {
        if (!PacketAlertCheck(p[i], 0)) {
            result = 0;
            goto end;
        }
        for (j = 1; j <= 10; j++) {
            if (j == i + 1) {
                if (!PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            } else {
                if (PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            }
        }
    }

 end:
    for (i = 0; i < no_of_pkts; i++) {
        SCFree(p[i]);
    }
    SCCudaPBCleanUpQueuesAndBuffers();
    if (de_ctx) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    SCCudaPBThreadDeInit(NULL, (void *)pb_tctx);
    B2gCudaMpmDispThreadDeInit(NULL, (void *)b2g_tctx);

    return result;
}

static int B2gCudaTest04(void)
{
    uint8_t raw_eth[] = {
        0x00, 0x25, 0x00, 0x9e, 0xfa, 0xfe, 0x00, 0x02,
        0xcf, 0x74, 0xfe, 0xe1, 0x08, 0x00, 0x45, 0x00,
        0x01, 0xcc, 0xcb, 0x91, 0x00, 0x00, 0x34, 0x06,
        0xdf, 0xa8, 0xd1, 0x55, 0xe3, 0x67, 0xc0, 0xa8,
        0x64, 0x8c, 0x00, 0x50, 0xc0, 0xb7, 0xd1, 0x11,
        0xed, 0x63, 0x81, 0xa9, 0x9a, 0x05, 0x80, 0x18,
        0x00, 0x75, 0x0a, 0xdd, 0x00, 0x00, 0x01, 0x01,
        0x08, 0x0a, 0x09, 0x8a, 0x06, 0xd0, 0x12, 0x21,
        0x2a, 0x3b, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31,
        0x2e, 0x31, 0x20, 0x33, 0x30, 0x32, 0x20, 0x46,
        0x6f, 0x75, 0x6e, 0x64, 0x0d, 0x0a, 0x4c, 0x6f,
        0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x0d, 0x0a, 0x43,
        0x61, 0x63, 0x68, 0x65, 0x2d, 0x43, 0x6f, 0x6e,
        0x74, 0x72, 0x6f, 0x6c, 0x3a, 0x20, 0x70, 0x72,
        0x69, 0x76, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x43,
        0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54,
        0x79, 0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x20,
        0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d,
        0x55, 0x54, 0x46, 0x2d, 0x38, 0x0d, 0x0a, 0x44,
        0x61, 0x74, 0x65, 0x3a, 0x20, 0x4d, 0x6f, 0x6e,
        0x2c, 0x20, 0x31, 0x34, 0x20, 0x53, 0x65, 0x70,
        0x20, 0x32, 0x30, 0x30, 0x39, 0x20, 0x30, 0x38,
        0x3a, 0x34, 0x38, 0x3a, 0x33, 0x31, 0x20, 0x47,
        0x4d, 0x54, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
        0x65, 0x72, 0x3a, 0x20, 0x67, 0x77, 0x73, 0x0d,
        0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
        0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a,
        0x20, 0x32, 0x31, 0x38, 0x0d, 0x0a, 0x0d, 0x0a,
        0x3c, 0x48, 0x54, 0x4d, 0x4c, 0x3e, 0x3c, 0x48,
        0x45, 0x41, 0x44, 0x3e, 0x3c, 0x6d, 0x65, 0x74,
        0x61, 0x20, 0x68, 0x74, 0x74, 0x70, 0x2d, 0x65,
        0x71, 0x75, 0x69, 0x76, 0x3d, 0x22, 0x63, 0x6f,
        0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x74, 0x79,
        0x70, 0x65, 0x22, 0x20, 0x63, 0x6f, 0x6e, 0x74,
        0x65, 0x6e, 0x74, 0x3d, 0x22, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x63,
        0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x75,
        0x74, 0x66, 0x2d, 0x38, 0x22, 0x3e, 0x0a, 0x3c,
        0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x33, 0x30,
        0x32, 0x20, 0x4d, 0x6f, 0x76, 0x65, 0x64, 0x3c,
        0x2f, 0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x3c,
        0x2f, 0x48, 0x45, 0x41, 0x44, 0x3e, 0x3c, 0x42,
        0x4f, 0x44, 0x59, 0x3e, 0x0a, 0x3c, 0x48, 0x31,
        0x3e, 0x33, 0x30, 0x32, 0x20, 0x4d, 0x6f, 0x76,
        0x65, 0x64, 0x3c, 0x2f, 0x48, 0x31, 0x3e, 0x0a,
        0x54, 0x68, 0x65, 0x20, 0x64, 0x6f, 0x63, 0x75,
        0x6d, 0x65, 0x6e, 0x74, 0x20, 0x68, 0x61, 0x73,
        0x20, 0x6d, 0x6f, 0x76, 0x65, 0x64, 0x0a, 0x3c,
        0x41, 0x20, 0x48, 0x52, 0x45, 0x46, 0x3d, 0x22,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x22, 0x3e, 0x68,
        0x65, 0x72, 0x65, 0x3c, 0x2f, 0x41, 0x3e, 0x2e,
        0x0d, 0x0a, 0x3c, 0x2f, 0x42, 0x4f, 0x44, 0x59,
        0x3e, 0x3c, 0x2f, 0x48, 0x54, 0x4d, 0x4c, 0x3e,
        0x0d, 0x0a };


    int result = 0;
    const char *strings[10] = {
        "test_test_one",
        "test_two_test",
        "test_three_test",
        "test_four_test",
        "test_five_test",
        "test_six_test",
        "test_seven_test",
        "test_eight_test",
        "test_nine_test",
        "test_ten_test"};
    /* don't shoot me for hardcoding the results.  We will change this in
     * sometime, by having run a separate mpm on the cpu and then hold
     * the results in a temp buffer */

    const uint16_t max_pkts_in_buffer = 5;
    const uint16_t no_of_pkts = max_pkts_in_buffer * 2;

    Packet *p[no_of_pkts];
    SCCudaPBThreadCtx *pb_tctx = NULL;

    DecodeThreadVars dtv;
    ThreadVars tv;
    DetectEngineCtx *de_ctx = NULL;
    DetectEngineThreadCtx *det_ctx;
    ThreadVars de_tv;

    SCCudaPBPacketsBuffer *pb = NULL;
    SCDQDataQueue *dq = NULL;

    char *inq_name = "cuda_batcher_mpm_inqueue";
    char *outq_name = "cuda_batcher_mpm_outqueue";

    Tmq *tmq_outq = NULL;
    Tmq *tmq_inq = NULL;

    uint32_t i = 0, j = 0;

    Signature *sig = NULL;

    memset(&dtv, 0, sizeof(DecodeThreadVars));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&de_tv, 0, sizeof(ThreadVars));

    FlowInitConfig(FLOW_QUIET);
    for (i = 0; i < no_of_pkts; i++) {
        p[i] = SCMalloc(sizeof(Packet));
        if (p[i] == NULL) {
            printf("error allocating memory\n");
            exit(EXIT_FAILURE);
        }
        memset(p[i], 0, sizeof(Packet));
        p[i]->pkt = (uint8_t *)(p[i] + 1);
        DecodeEthernet(&tv, &dtv, p[i], raw_eth, sizeof(raw_eth), NULL);
    }

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G_CUDA;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                               "content:\"test\"; sid:0;)");
    if (de_ctx->sig_list == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = de_ctx->sig_list;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"one\"; sid:1;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"two\"; sid:2;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"three\"; sid:3;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"four\"; sid:4;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"five\"; sid:5;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"six\"; sid:6;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"seven\"; sid:7;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"eight\"; sid:8;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"nine\"; sid:9;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"ten\"; sid:10;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }

    /* build the signatures */
    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&de_tv, (void *)de_ctx, (void *)&det_ctx);

    SCCudaPBSetProfile("mpm");
    SCCudaPBSetUpQueuesAndBuffers();

    /* get the queues used by the batcher thread */
    tmq_inq = TmqGetQueueByName(inq_name);
    if (tmq_inq == NULL) {
        printf("tmq_inq NULL\n");
        goto end;
    }
    tmq_outq = TmqGetQueueByName(outq_name);
    if (tmq_outq == NULL) {
        printf("tmq_outq NULL\n");
        goto end;
    }

    result = 1;

    /* queue state before calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 10);

    SCCudaPBRunningTests(1);
    /* init the TM thread */
    SCCudaPBThreadInit(&tv, de_ctx, (void *)&pb_tctx);
    SCCudaPBSetBufferPacketThreshhold(max_pkts_in_buffer);

    /* queue state after calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 9);

    for (i = 0; i < no_of_pkts; i++) {
        p[i]->payload = (uint8_t *)strings[i];
        p[i]->payload_len = strlen(strings[i]);
        SCCudaPBBatchPackets(NULL, p[i], pb_tctx, NULL, NULL);
    }

    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 2);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 7);

    int module_handle = SCCudaHlRegisterModule("SC_RULES_CONTENT_B2G_CUDA");
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(module_handle);

    B2gCudaMpmThreadCtxData *b2g_tctx = NULL;
    B2gCudaMpmDispThreadInit(NULL, module_data, (void *)&b2g_tctx);

    if (b2g_tctx->no_of_streams < 2) {
        printf("At least 2 cuda streams needed for this test. Skipping ..\n");
        goto end;
    }

    if (b2g_tctx->b2g_cuda_context == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_cumodule == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_search_kernel == 0) {
        result = 0;
        goto end;
    }

    SCCudaCtxSynchronize();

    /* Run the dispatcher function. */
    pb = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[tmq_outq->id]);
    if (pb == NULL) {
        SCLogError(SC_ERR_INVALID_VALUE, "PacketBuffer should not be empty");
        result = 0;
    }
    result &= (pb->nop_in_buffer == max_pkts_in_buffer);
    B2gCudaMpmDispatcher(NULL, (Packet *)pb, b2g_tctx, (PacketQueue *)&data_queues[tmq_outq->id], NULL);
    TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb);
    if (data_queues[b2g_tctx->tmq_streamq->id].len != 1) {
        result = 0;
    }
    while ((pb = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[b2g_tctx->tmq_streamq->id])) != NULL) {
        TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb);
    }
    if (data_queues[b2g_tctx->tmq_streamq->id].len != 0) {
        result = 0;
    }

    for (i = 0; i < no_of_pkts; i++)
        SigMatchSignatures(&de_tv, de_ctx, det_ctx, p[i]);
    for (i = 0; i < no_of_pkts; i++) {
        if (!PacketAlertCheck(p[i], 0)) {
            result = 0;
            goto end;
        }
        for (j = 1; j <= 10; j++) {
            if (j == i + 1) {
                if (!PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            } else {
                if (PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            }
        }
    }

 end:
    for (i = 0; i < no_of_pkts; i++) {
        SCFree(p[i]);
    }
    SCCudaPBCleanUpQueuesAndBuffers();
    if (de_ctx) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    SCCudaPBThreadDeInit(NULL, (void *)pb_tctx);
    B2gCudaMpmDispThreadDeInit(NULL, (void *)b2g_tctx);

    return result;
}

static int B2gCudaTest05(void)
{
    uint8_t raw_eth[] = {
        0x00, 0x25, 0x00, 0x9e, 0xfa, 0xfe, 0x00, 0x02,
        0xcf, 0x74, 0xfe, 0xe1, 0x08, 0x00, 0x45, 0x00,
        0x01, 0xcc, 0xcb, 0x91, 0x00, 0x00, 0x34, 0x06,
        0xdf, 0xa8, 0xd1, 0x55, 0xe3, 0x67, 0xc0, 0xa8,
        0x64, 0x8c, 0x00, 0x50, 0xc0, 0xb7, 0xd1, 0x11,
        0xed, 0x63, 0x81, 0xa9, 0x9a, 0x05, 0x80, 0x18,
        0x00, 0x75, 0x0a, 0xdd, 0x00, 0x00, 0x01, 0x01,
        0x08, 0x0a, 0x09, 0x8a, 0x06, 0xd0, 0x12, 0x21,
        0x2a, 0x3b, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x31,
        0x2e, 0x31, 0x20, 0x33, 0x30, 0x32, 0x20, 0x46,
        0x6f, 0x75, 0x6e, 0x64, 0x0d, 0x0a, 0x4c, 0x6f,
        0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x0d, 0x0a, 0x43,
        0x61, 0x63, 0x68, 0x65, 0x2d, 0x43, 0x6f, 0x6e,
        0x74, 0x72, 0x6f, 0x6c, 0x3a, 0x20, 0x70, 0x72,
        0x69, 0x76, 0x61, 0x74, 0x65, 0x0d, 0x0a, 0x43,
        0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x54,
        0x79, 0x70, 0x65, 0x3a, 0x20, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x20,
        0x63, 0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d,
        0x55, 0x54, 0x46, 0x2d, 0x38, 0x0d, 0x0a, 0x44,
        0x61, 0x74, 0x65, 0x3a, 0x20, 0x4d, 0x6f, 0x6e,
        0x2c, 0x20, 0x31, 0x34, 0x20, 0x53, 0x65, 0x70,
        0x20, 0x32, 0x30, 0x30, 0x39, 0x20, 0x30, 0x38,
        0x3a, 0x34, 0x38, 0x3a, 0x33, 0x31, 0x20, 0x47,
        0x4d, 0x54, 0x0d, 0x0a, 0x53, 0x65, 0x72, 0x76,
        0x65, 0x72, 0x3a, 0x20, 0x67, 0x77, 0x73, 0x0d,
        0x0a, 0x43, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74,
        0x2d, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x3a,
        0x20, 0x32, 0x31, 0x38, 0x0d, 0x0a, 0x0d, 0x0a,
        0x3c, 0x48, 0x54, 0x4d, 0x4c, 0x3e, 0x3c, 0x48,
        0x45, 0x41, 0x44, 0x3e, 0x3c, 0x6d, 0x65, 0x74,
        0x61, 0x20, 0x68, 0x74, 0x74, 0x70, 0x2d, 0x65,
        0x71, 0x75, 0x69, 0x76, 0x3d, 0x22, 0x63, 0x6f,
        0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2d, 0x74, 0x79,
        0x70, 0x65, 0x22, 0x20, 0x63, 0x6f, 0x6e, 0x74,
        0x65, 0x6e, 0x74, 0x3d, 0x22, 0x74, 0x65, 0x78,
        0x74, 0x2f, 0x68, 0x74, 0x6d, 0x6c, 0x3b, 0x63,
        0x68, 0x61, 0x72, 0x73, 0x65, 0x74, 0x3d, 0x75,
        0x74, 0x66, 0x2d, 0x38, 0x22, 0x3e, 0x0a, 0x3c,
        0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x33, 0x30,
        0x32, 0x20, 0x4d, 0x6f, 0x76, 0x65, 0x64, 0x3c,
        0x2f, 0x54, 0x49, 0x54, 0x4c, 0x45, 0x3e, 0x3c,
        0x2f, 0x48, 0x45, 0x41, 0x44, 0x3e, 0x3c, 0x42,
        0x4f, 0x44, 0x59, 0x3e, 0x0a, 0x3c, 0x48, 0x31,
        0x3e, 0x33, 0x30, 0x32, 0x20, 0x4d, 0x6f, 0x76,
        0x65, 0x64, 0x3c, 0x2f, 0x48, 0x31, 0x3e, 0x0a,
        0x54, 0x68, 0x65, 0x20, 0x64, 0x6f, 0x63, 0x75,
        0x6d, 0x65, 0x6e, 0x74, 0x20, 0x68, 0x61, 0x73,
        0x20, 0x6d, 0x6f, 0x76, 0x65, 0x64, 0x0a, 0x3c,
        0x41, 0x20, 0x48, 0x52, 0x45, 0x46, 0x3d, 0x22,
        0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x67, 0x6f, 0x6f, 0x67, 0x6c,
        0x65, 0x2e, 0x65, 0x73, 0x2f, 0x22, 0x3e, 0x68,
        0x65, 0x72, 0x65, 0x3c, 0x2f, 0x41, 0x3e, 0x2e,
        0x0d, 0x0a, 0x3c, 0x2f, 0x42, 0x4f, 0x44, 0x59,
        0x3e, 0x3c, 0x2f, 0x48, 0x54, 0x4d, 0x4c, 0x3e,
        0x0d, 0x0a };


    int result = 0;
    const char *strings[10] = {
        "test_test_one",
        "test_two_test",
        "test_three_test",
        "test_four_test",
        "test_five_test",
        "test_six_test",
        "test_seven_test",
        "test_eight_test",
        "test_nine_test",
        "test_ten_test"};
    /* don't shoot me for hardcoding the results.  We will change this in
     * sometime, by having run a separate mpm on the cpu and then hold
     * the results in a temp buffer */

    const uint16_t max_pkts_in_buffer = 300;
    const uint16_t no_of_pkts = max_pkts_in_buffer * 4;
    uint16_t max_runs = 2;

    Packet *p[no_of_pkts];
    SCCudaPBThreadCtx *pb_tctx = NULL;

    DecodeThreadVars dtv;
    ThreadVars tv;
    DetectEngineCtx *de_ctx = NULL;
    DetectEngineThreadCtx *det_ctx;
    ThreadVars de_tv;

    SCCudaPBPacketsBuffer *pb = NULL;
    SCCudaPBPacketsBuffer *pb2 = NULL;
    SCDQDataQueue *dq = NULL;

    char *inq_name = "cuda_batcher_mpm_inqueue";
    char *outq_name = "cuda_batcher_mpm_outqueue";

    Tmq *tmq_outq = NULL;
    Tmq *tmq_inq = NULL;

    uint32_t i = 0, j = 0;

    Signature *sig = NULL;

    memset(&dtv, 0, sizeof(DecodeThreadVars));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&de_tv, 0, sizeof(ThreadVars));

    FlowInitConfig(FLOW_QUIET);
    for (i = 0; i < no_of_pkts; i++) {
        p[i] = SCMalloc(sizeof(Packet));
        if (p[i] == NULL) {
            printf("error allocating memory\n");
            exit(EXIT_FAILURE);
        }
        memset(p[i], 0, sizeof(Packet));
        p[i]->pkt = (uint8_t *)(p[i] + 1);
        DecodeEthernet(&tv, &dtv, p[i], raw_eth, sizeof(raw_eth), NULL);
    }

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G_CUDA;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                               "content:\"test\"; sid:0;)");
    if (de_ctx->sig_list == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = de_ctx->sig_list;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"one\"; sid:1;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"two\"; sid:2;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"three\"; sid:3;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"four\"; sid:4;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"five\"; sid:5;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"six\"; sid:6;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"seven\"; sid:7;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"eight\"; sid:8;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"nine\"; sid:9;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }
    sig = sig->next;

    sig->next = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Bamboo\"; "
                        "content:\"ten\"; sid:10;)");
    if (sig->next == NULL) {
        printf("signature parsing failed\n");
        goto end;
    }

    /* build the signatures */
    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&de_tv, (void *)de_ctx, (void *)&det_ctx);

    SCCudaPBSetProfile("mpm");
    SCCudaPBSetUpQueuesAndBuffers();

    /* get the queues used by the batcher thread */
    tmq_inq = TmqGetQueueByName(inq_name);
    if (tmq_inq == NULL) {
        printf("tmq_inq NULL\n");
        goto end;
    }
    tmq_outq = TmqGetQueueByName(outq_name);
    if (tmq_outq == NULL) {
        printf("tmq_outq NULL\n");
        goto end;
    }

    result = 1;

    /* queue state before calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 10);

    SCCudaPBRunningTests(1);
    /* init the TM thread */
    SCCudaPBThreadInit(&tv, de_ctx, (void *)&pb_tctx);
    SCCudaPBSetBufferPacketThreshhold(max_pkts_in_buffer);

    /* queue state after calling the thread init function */
    dq = &data_queues[tmq_outq->id];
    result &= (dq->len == 0);
    dq = &data_queues[tmq_inq->id];
    result &= (dq->len == 9);

    int module_handle = SCCudaHlRegisterModule("SC_RULES_CONTENT_B2G_CUDA");
    SCCudaHlModuleData *module_data = SCCudaHlGetModuleData(module_handle);

    B2gCudaMpmThreadCtxData *b2g_tctx = NULL;
    B2gCudaMpmDispThreadInit(NULL, module_data, (void *)&b2g_tctx);

    if (b2g_tctx->no_of_streams < 2) {
        printf("At least 2 cuda streams needed for this test. Skipping ..\n");
        goto end;
    }

    if (b2g_tctx->b2g_cuda_context == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_cumodule == 0 ||
        b2g_tctx->stream_data[0].b2g_cuda_search_kernel == 0) {
        result = 0;
        goto end;
    }

    uint64_t start_ = 0;
    uint64_t stop_ = 0;
    uint64_t time_nostream_ = 0;
    uint64_t time_stream_ = 0;

    SCCudaCtxSynchronize();

    /* Benchmark the dispatcher function. */
    int run = 0;
    for (run = 0; run < max_runs; ++run) {
        /* Fill the packet buffers */
        for (i = 0; i < no_of_pkts; i++) {
            p[i]->payload = (uint8_t *)strings[i%10];
            p[i]->payload_len = strlen(strings[i%10]);
            SCCudaPBBatchPackets(NULL, p[i], pb_tctx, NULL, NULL);
        }

        dq = &data_queues[tmq_outq->id];
        result &= (dq->len == 4);
        dq = &data_queues[tmq_inq->id];
        result &= (dq->len == 5);

        /* Performance test with two separate calls */
        pb = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[tmq_outq->id]);
        pb2 = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[tmq_outq->id]);
        if (pb == NULL || pb2 == NULL) {
            SCLogError(SC_ERR_INVALID_VALUE, "PacketBuffer should not be empty");
            result = 0;
        }
        result &= (pb->nop_in_buffer == max_pkts_in_buffer);
        result &= (pb2->nop_in_buffer == max_pkts_in_buffer);
        start_ = UtilCpuGetTicks();
        B2gCudaMpmDispatcher(NULL, (Packet *)pb, b2g_tctx, (PacketQueue *)NULL, (PacketQueue *)NULL);
        B2gCudaMpmDispatcher(NULL, (Packet *)pb2, b2g_tctx, (PacketQueue *)NULL, (PacketQueue *)NULL);
        stop_ = UtilCpuGetTicks();
        TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb);
        TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb2);
        time_nostream_ += stop_ - start_;

        /* Performance test with one call using streams */
        pb = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[tmq_outq->id]);
        if (pb == NULL) {
            SCLogError(SC_ERR_INVALID_VALUE, "PacketBuffer should not be empty");
            result = 0;
        }
        result &= (pb->nop_in_buffer == max_pkts_in_buffer);
        result &= (pb2->nop_in_buffer == max_pkts_in_buffer);
        start_ = UtilCpuGetTicks();
        B2gCudaMpmDispatcher(NULL, (Packet *)pb, b2g_tctx, (PacketQueue *)&data_queues[tmq_outq->id], NULL);
        stop_ = UtilCpuGetTicks();
        TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb);
        while ((pb = (SCCudaPBPacketsBuffer *)SCDQDataDequeue(&data_queues[b2g_tctx->tmq_streamq->id])) != NULL) {
            TmqhOutputSimpleOnQ(&data_queues[tmq_inq->id], (SCDQGenericQData *)pb);
        }
        time_stream_ += stop_ - start_;
    }

    printf("Avg CPU ticks without stream after %i runs: %"PRIu64"\n", run, time_nostream_/run);
    printf("Avg CPU ticks with 2 streams after %i runs: %"PRIu64"\n", run, time_stream_/run);

    for (i = 0; i < no_of_pkts; i++)
        SigMatchSignatures(&de_tv, de_ctx, det_ctx, p[i]);
    for (i = 0; i < no_of_pkts; i++) {
        if (!PacketAlertCheck(p[i], 0)) {
            result = 0;
            goto end;
        }
        for (j = 1; j <= 10; j++) {
            if (j == i%10 + 1) {
                if (!PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            } else {
                if (PacketAlertCheck(p[i], j)) {
                    result = 0;
                    goto end;
                }
            }
        }
    }

 end:
    for (i = 0; i < no_of_pkts; i++) {
        SCFree(p[i]);
    }
    SCCudaPBCleanUpQueuesAndBuffers();
    if (de_ctx) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    SCCudaPBThreadDeInit(NULL, (void *)pb_tctx);
    B2gCudaMpmDispThreadDeInit(NULL, (void *)b2g_tctx);

    return result;
}

#endif /* UNITTESTS */

/*********************************Unittests************************************/

void B2gCudaRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("B2gCudaTest01", B2gCudaTest01, 1);
    UtRegisterTest("B2gCudaTest02", B2gCudaTest02, 1);
    UtRegisterTest("B2gCudaTest03", B2gCudaTest03, 1);
    UtRegisterTest("B2gCudaTest04", B2gCudaTest04, 1);
    UtRegisterTest("B2gCudaTest05", B2gCudaTest05, 1);
#endif /* UNITTESTS */
}

#endif /* __SC_CUDA_SUPPORT */
