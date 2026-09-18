// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_hash.h"
#include "common/common.h"
#include <string.h>
#include <stdio.h>

static const uint8_t MD5_PADDING[64] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

#define MD5_FF(a, b, c, d, x, s, ac) {(a) += MD5_F ((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT ((a), (s)); (a) += (b); }
#define MD5_GG(a, b, c, d, x, s, ac) {(a) += MD5_G ((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT ((a), (s)); (a) += (b); }
#define MD5_HH(a, b, c, d, x, s, ac) {(a) += MD5_H ((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT ((a), (s)); (a) += (b); }
#define MD5_II(a, b, c, d, x, s, ac) {(a) += MD5_I ((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT ((a), (s)); (a) += (b); }

#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

typedef struct {
    uint32_t i[2];
    uint32_t buf[4];
    unsigned char in[64];
} MD5_CTX;

static void md5_transform(uint32_t* buf, uint32_t* in) {
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

    /* Round 1 */
    MD5_FF(a, b, c, d, in[ 0], MD5_S11, 3614090360u);
    MD5_FF(d, a, b, c, in[ 1], MD5_S12, 3905402710u);
    MD5_FF(c, d, a, b, in[ 2], MD5_S13,  606105819u);
    MD5_FF(b, c, d, a, in[ 3], MD5_S14, 3250441966u);
    MD5_FF(a, b, c, d, in[ 4], MD5_S11, 4118548399u);
    MD5_FF(d, a, b, c, in[ 5], MD5_S12, 1200080426u);
    MD5_FF(c, d, a, b, in[ 6], MD5_S13, 2821735955u);
    MD5_FF(b, c, d, a, in[ 7], MD5_S14, 4249261313u);
    MD5_FF(a, b, c, d, in[ 8], MD5_S11, 1770035416u);
    MD5_FF(d, a, b, c, in[ 9], MD5_S12, 2336552879u);
    MD5_FF(c, d, a, b, in[10], MD5_S13, 4294925233u);
    MD5_FF(b, c, d, a, in[11], MD5_S14, 2304563134u);
    MD5_FF(a, b, c, d, in[12], MD5_S11, 1804603682u);
    MD5_FF(d, a, b, c, in[13], MD5_S12, 4254626195u);
    MD5_FF(c, d, a, b, in[14], MD5_S13, 2792965006u);
    MD5_FF(b, c, d, a, in[15], MD5_S14, 1236535329u);

    /* Round 2 */
    MD5_GG(a, b, c, d, in[ 1], MD5_S21, 4129170786u);
    MD5_GG(d, a, b, c, in[ 6], MD5_S22, 3225465664u);
    MD5_GG(c, d, a, b, in[11], MD5_S23,  643717713u);
    MD5_GG(b, c, d, a, in[ 0], MD5_S24, 3921069994u);
    MD5_GG(a, b, c, d, in[ 5], MD5_S21, 3593408605u);
    MD5_GG(d, a, b, c, in[10], MD5_S22,   38016083u);
    MD5_GG(c, d, a, b, in[15], MD5_S23, 3634488961u);
    MD5_GG(b, c, d, a, in[ 4], MD5_S24, 3889429448u);
    MD5_GG(a, b, c, d, in[ 9], MD5_S21,  568446438u);
    MD5_GG(d, a, b, c, in[14], MD5_S22, 3275163606u);
    MD5_GG(c, d, a, b, in[ 3], MD5_S23, 4107603335u);
    MD5_GG(b, c, d, a, in[ 8], MD5_S24, 1163531501u);
    MD5_GG(a, b, c, d, in[13], MD5_S21, 2850285829u);
    MD5_GG(d, a, b, c, in[ 2], MD5_S22, 4243563512u);
    MD5_GG(c, d, a, b, in[ 7], MD5_S23, 1735328473u);
    MD5_GG(b, c, d, a, in[12], MD5_S24, 2368359562u);

    /* Round 3 */
    MD5_HH(a, b, c, d, in[ 5], MD5_S31, 4294588738u);
    MD5_HH(d, a, b, c, in[ 8], MD5_S32, 2272392833u);
    MD5_HH(c, d, a, b, in[11], MD5_S33, 1839030562u);
    MD5_HH(b, c, d, a, in[14], MD5_S34, 4259657740u);
    MD5_HH(a, b, c, d, in[ 1], MD5_S31, 2763975236u);
    MD5_HH(d, a, b, c, in[ 4], MD5_S32, 1272893353u);
    MD5_HH(c, d, a, b, in[ 7], MD5_S33, 4139469664u);
    MD5_HH(b, c, d, a, in[10], MD5_S34, 3200236656u);
    MD5_HH(a, b, c, d, in[13], MD5_S31,  681279174u);
    MD5_HH(d, a, b, c, in[ 0], MD5_S32, 3936430074u);
    MD5_HH(c, d, a, b, in[ 3], MD5_S33, 3572445317u);
    MD5_HH(b, c, d, a, in[ 6], MD5_S34,   76029189u);
    MD5_HH(a, b, c, d, in[ 9], MD5_S31, 3654602809u);
    MD5_HH(d, a, b, c, in[12], MD5_S32, 3873151461u);
    MD5_HH(c, d, a, b, in[15], MD5_S33,  530742520u);
    MD5_HH(b, c, d, a, in[ 2], MD5_S34, 3299628645u);

    /* Round 4 */
    MD5_II(a, b, c, d, in[ 0], MD5_S41, 4096336452u);
    MD5_II(d, a, b, c, in[ 7], MD5_S42, 1126891415u);
    MD5_II(c, d, a, b, in[14], MD5_S43, 2878612391u);
    MD5_II(b, c, d, a, in[ 5], MD5_S44, 4237533241u);
    MD5_II(a, b, c, d, in[12], MD5_S41, 1700485571u);
    MD5_II(d, a, b, c, in[ 3], MD5_S42, 2399980690u);
    MD5_II(c, d, a, b, in[10], MD5_S43, 4293915773u);
    MD5_II(b, c, d, a, in[ 1], MD5_S44, 2240044497u);
    MD5_II(a, b, c, d, in[ 8], MD5_S41, 1873313359u);
    MD5_II(d, a, b, c, in[15], MD5_S42, 4264355552u);
    MD5_II(c, d, a, b, in[ 6], MD5_S43, 2734768916u);
    MD5_II(b, c, d, a, in[13], MD5_S44, 1309151649u);
    MD5_II(a, b, c, d, in[ 4], MD5_S41, 4149444226u);
    MD5_II(d, a, b, c, in[11], MD5_S42, 3174756917u);
    MD5_II(c, d, a, b, in[ 2], MD5_S43,  718787259u);
    MD5_II(b, c, d, a, in[ 9], MD5_S44, 3951481745u);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

static void md5_init(MD5_CTX* mdContext) {
    mdContext->i[0] = mdContext->i[1] = 0;
    mdContext->buf[0] = 0x67452301;
    mdContext->buf[1] = 0xefcdab89;
    mdContext->buf[2] = 0x98badcfe;
    mdContext->buf[3] = 0x10325476;
}

static void md5_update(MD5_CTX* mdContext, const uint8_t* inBuf, unsigned int inLen) {
    uint32_t in[16];
    int mdi = 0;
    unsigned int i = 0, ii = 0;

    mdi = (int)((mdContext->i[0] >> 3) & 0x3F);

    if ((mdContext->i[0] + ((uint32_t)inLen << 3)) < mdContext->i[0]) {
        mdContext->i[1]++;
    }

    mdContext->i[0] += ((uint32_t)inLen << 3);
    mdContext->i[1] += ((uint32_t)inLen >> 29);

    while (inLen--) {
        mdContext->in[mdi++] = *inBuf++;

        if (mdi == 0x40) {
            for (i = 0, ii = 0; i < 16; i++, ii += 4) {
                in[i] = (((uint32_t)mdContext->in[ii + 3]) << 24) |
                        (((uint32_t)mdContext->in[ii + 2]) << 16) |
                        (((uint32_t)mdContext->in[ii + 1]) << 8) |
                        ((uint32_t)mdContext->in[ii]);
            }

            md5_transform(mdContext->buf, in);
            mdi = 0;
        }
    }
}

static void calculate_dxbc_checksum(const uint8_t* pData, uint32_t dwSize, uint32_t dwHash[4]) {
    MD5_CTX md5Ctx;
    md5_init(&md5Ctx);

    const uint32_t dwHashOffset = 20;
    dwSize -= dwHashOffset;
    pData += dwHashOffset;

    uint32_t dwNumberOfBits = dwSize * 8;
    uint32_t dwFullChunksSize = dwSize & 0xffffffc0;
    md5_update(&md5Ctx, pData, dwFullChunksSize);

    uint32_t dwLastChunkSize = dwSize - dwFullChunksSize;
    uint32_t dwPaddingSize = 64 - dwLastChunkSize;
    const uint8_t* pLastChunkData = pData + dwFullChunksSize;

    if (dwLastChunkSize >= 56) {
        md5_update(&md5Ctx, pLastChunkData, dwLastChunkSize);
        md5_update(&md5Ctx, MD5_PADDING, dwPaddingSize);

        uint32_t in[16];
        memset(in, 0, sizeof(in));
        in[0] = dwNumberOfBits;
        in[15] = (dwNumberOfBits >> 2) | 1;

        md5_transform(md5Ctx.buf, in);
    } else {
        md5_update(&md5Ctx, (const uint8_t*)&dwNumberOfBits, 4);

        if (dwLastChunkSize) {
            md5_update(&md5Ctx, pLastChunkData, dwLastChunkSize);
        }

        dwLastChunkSize += 4;
        dwPaddingSize -= 4;

        memcpy(&md5Ctx.in[dwLastChunkSize], MD5_PADDING, dwPaddingSize);

        ((uint32_t*)md5Ctx.in)[15] = (dwNumberOfBits >> 2) | 1;

        uint32_t in[16];
        memcpy(in, md5Ctx.in, 64);

        md5_transform(md5Ctx.buf, in);
    }

    memcpy(dwHash, md5Ctx.buf, 16);
}

bool dxbc_compute_hash(const uint8_t* data, size_t size,
                       uint8_t out_hash[16]) {
    if (!data || !out_hash || size < 32) {
        return false;
    }

    // Read total size from the container header at offset 24
    uint32_t total_size_raw;
    memcpy(&total_size_raw, data + 24, sizeof(total_size_raw));
    uint32_t total_size = read_le32(total_size_raw);
    if (total_size < 32 || total_size > size) {
        return false;
    }

    uint32_t computed_hash[4];
    calculate_dxbc_checksum(data, total_size, computed_hash);
    memcpy(out_hash, computed_hash, 16);
    return true;
}

bool dxbc_verify_hash(const uint8_t* data, size_t size) {
    if (!data || size < 32) {
        return false;
    }
    uint8_t computed_hash[16];
    if (!dxbc_compute_hash(data, size, computed_hash)) {
        return false;
    }
    
    uint32_t expected_hash[4];
    memcpy(expected_hash, data + 4, 16);
    return memcmp(expected_hash, computed_hash, 16) == 0;
}
