//
// 3dsfontprovider.cpp
//
// 从 3DS 系统共享字体中按 Unicode 码位提取字形位图（CPU 端采样）。
//
// 已验证的字体规格（本机实测，见 probe_report.txt）：
//   cellWidth = 21   cellHeight = 24   baselinePos = 19
//   nRows = 23  nLines = 5  nSheets = 98
//   sheet 512 x 128，sheetFmt = GPU_A4（4bpp 纯 alpha）
//   字形在 sheet 中的位置：x = rowId  * (cellWidth  + 1) + 1
//                          y = lineId * (cellHeight + 1) + 1
//   （与 libctru fontCalcGlyphPos() 内部算法一致）
//
// 贴图为 3DS GPU 原生布局：8x8 tile，tile 内 Morton 序。
//

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <3ds.h>
#include <3ds/font.h>
#include <3ds/gpu/enums.h>

#include "3dstypes.h"
#include "3dsfontprovider.h"

//-----------------------------------------------------------------------
// 配置
//-----------------------------------------------------------------------
#define GLYPH_CACHE_SLOTS       2048            // 字形缓存槽数
#define GLYPH_CACHE_MASK        (GLYPH_CACHE_SLOTS - 1)
#define GLYPH_PROBE_LIMIT       8               // 开放寻址最大探测长度
#define GLYPH_POOL_BYTES        (1024 * 1024)   // 字形位图池：1MB
#define GLYPH_MAX_PIXELS        (40 * 40)       // 单字形上限（用于临时缓冲）

// 覆盖率"描边加深"（stem darkening）指数。取 1.0 表示关闭。
//
// 系统字体是 24px 轮廓字，缩到 13px 只有 0.54 倍：笔画宽度不足 1px，
// 按面积线性平均后得到的覆盖率必然偏淡，观感就是"发虚、不清晰"。
// 所有小字光栅化器都要处理这个问题（Windows/FreeType 都做同类补偿），
// 做法是把覆盖率做一次幂次抬升：cov' = cov^gamma (gamma < 1)，
// 中间调被抬高、笔画变实，同时保留边缘的一个过渡像素（不会变成锯齿）。
#define GLYPH_DARKEN_GAMMA      0.72f

static uint8  s_covLut[256];
static bool   s_covLutReady = false;

static void ensureCovLut(void)
{
    if (s_covLutReady) return;

    for (int i = 0; i < 256; i++)
    {
        float c = (float)i / 255.0f;
        float v = (GLYPH_DARKEN_GAMMA >= 0.999f) ? c : powf(c, GLYPH_DARKEN_GAMMA);
        int q = (int)(v * 255.0f + 0.5f);
        if (q < 0) q = 0;
        if (q > 255) q = 255;
        s_covLut[i] = (uint8)q;
    }
    s_covLutReady = true;
}

// 绘制基准微调（真机上用于中英混排基线对齐）
#define GLYPH_OFFY_ADJUST       0

//-----------------------------------------------------------------------
// 内部状态
//-----------------------------------------------------------------------
typedef struct
{
    uint32 codePoint;   // 0 = 空槽（U+0000 不参与缓存）
    uint32 offset;      // 池内字节偏移
    uint8  w;           // 位图宽（像素）
    uint8  h;           // 位图高（像素）
    int8   offX;        // 相对笔位的水平偏移
    int8   offY;        // 相对行顶的垂直偏移
    uint8  advance;     // 笔位步进
    uint8  pad;
} GlyphCacheEntry;

static CFNT_s          *s_font      = NULL;
static bool             s_ready     = false;
static bool             s_initTried = false;
static int              s_targetH   = 16;
static float            s_scale     = 1.0f;
static uint16           s_sheetFmt  = 0;

static GlyphCacheEntry *s_cache     = NULL;
static uint8           *s_pool      = NULL;
static uint32           s_poolUsed  = 0;

// 直通渲染用的临时缓冲（缓存池耗尽或 U+0000 时使用）
static uint8            s_scratch[GLYPH_MAX_PIXELS];

static int              s_hits      = 0;
static int              s_misses    = 0;
static int              s_missing   = 0;

#define MAX_MISSING     256
static uint32           s_missingList[MAX_MISSING];
static int              s_missingCount = 0;

//-----------------------------------------------------------------------
// GPU 纹理的 Morton 采样
//
// 3DS 纹理按 8x8 tile 排列，tile 内为 Morton（Z 序）。
// 4bpp 时两个 texel 共用一个字节，偶 texel 在低半字节。
//-----------------------------------------------------------------------
static inline int morton3(int a, int b)
{
    return ((a & 1) | ((b & 1) << 1) | ((a & 2) << 1) | ((b & 2) << 2)
            | ((a & 4) << 2) | ((b & 4) << 3));
}

// 返回 0..255 的 alpha
static inline uint8 glyphAlpha(const uint8 *sheet, int sheetWidth, int x, int y)
{
    int tilesPerRow = sheetWidth >> 3;
    int tile  = ((y >> 3) * tilesPerRow) + (x >> 3);
    int texel = (tile << 6) + morton3(x & 7, y & 7);

    switch (s_sheetFmt)
    {
    case GPU_A4:
    case GPU_LA4:
    {
        uint8 b = sheet[texel >> 1];
        uint8 a = (texel & 1) ? (uint8)(b >> 4) : (uint8)(b & 0x0F);
        return (uint8)(a * 17);
    }
    case GPU_A8:
        return sheet[texel];
    case GPU_LA8:
        return sheet[texel * 2 + 1];
    case GPU_RGBA8:
        return sheet[texel * 4 + 3];
    case GPU_RGBA5551:
    {
        uint16 v = ((const uint16 *)sheet)[texel];
        return (v & 0x0001) ? (uint8)255 : (uint8)0;
    }
    default:
        return (uint8)255;   // 未知格式：视作不透明
    }
}

//-----------------------------------------------------------------------
// 面积平均（box filter）下采样。
// 源为 sheet 上的绝对坐标区域，输出紧凑位图，alpha 压到 UI3DS_GLYPH_ALPHA_MAX。
//-----------------------------------------------------------------------
static void scaleGlyphArea(const uint8 *sheet, int sheetWidth,
                           int sx, int sy, int sw, int sh,
                           uint8 *dst, int dw, int dh)
{
    ensureCovLut();

    for (int oy = 0; oy < dh; oy++)
    {
        float fy0 = (float)sy + (float)oy * (float)sh / (float)dh;
        float fy1 = (float)sy + (float)(oy + 1) * (float)sh / (float)dh;
        int iy0 = (int)fy0;
        int iy1 = (int)fy1;
        if ((float)iy1 >= fy1) iy1--;
        if (iy1 > sy + sh - 1) iy1 = sy + sh - 1;

        for (int ox = 0; ox < dw; ox++)
        {
            float fx0 = (float)sx + (float)ox * (float)sw / (float)dw;
            float fx1 = (float)sx + (float)(ox + 1) * (float)sw / (float)dw;
            int ix0 = (int)fx0;
            int ix1 = (int)fx1;
            if ((float)ix1 >= fx1) ix1--;
            if (ix1 > sx + sw - 1) ix1 = sx + sw - 1;

            float acc  = 0.0f;
            float area = 0.0f;

            for (int y = iy0; y <= iy1; y++)
            {
                float cy0 = ((float)y > fy0) ? (float)y : fy0;
                float cy1 = ((float)(y + 1) < fy1) ? (float)(y + 1) : fy1;
                if (cy1 <= cy0) continue;

                for (int x = ix0; x <= ix1; x++)
                {
                    float cx0 = ((float)x > fx0) ? (float)x : fx0;
                    float cx1 = ((float)(x + 1) < fx1) ? (float)(x + 1) : fx1;
                    if (cx1 <= cx0) continue;

                    float a = (cy1 - cy0) * (cx1 - cx0);
                    acc  += (float)glyphAlpha(sheet, sheetWidth, x, y) * a;
                    area += a;
                }
            }

            // 覆盖率 0..255 -> 描边加深 -> 量化到 0..UI3DS_GLYPH_ALPHA_MAX
            int cov = 0;
            if (area > 0.0f)
            {
                cov = (int)((acc / area) + 0.5f);
                if (cov < 0)   cov = 0;
                if (cov > 255) cov = 255;
            }

            int v = (int)((s_covLut[cov] * UI3DS_GLYPH_ALPHA_MAX + 127) / 255);

            if (v < 0) v = 0;
            if (v > UI3DS_GLYPH_ALPHA_MAX) v = UI3DS_GLYPH_ALPHA_MAX;
            dst[oy * dw + ox] = (uint8)v;
        }
    }
}

//-----------------------------------------------------------------------
// 构建字形。
// dst 至少需 GLYPH_MAX_PIXELS 字节。成功返回 true 并填好 ent 的几何字段。
// 字形为空白（如空格）时返回 true 且 w = h = 0。
//-----------------------------------------------------------------------
static bool buildGlyph(uint32 codePoint, GlyphCacheEntry *ent, uint8 *dst)
{
    if (s_font == NULL) return false;

    int gi = fontGlyphIndexFromCodePoint(s_font, codePoint);
    if (gi < 0) return false;

    // 系统字体用"替代字形"表示缺字，据此判定该码位是否真的有字形
    if (gi == (int)s_font->finf.alterCharIndex) return false;

    const TGLP_s *tglp = s_font->finf.tglp;
    if (tglp == NULL) return false;

    int cellW = (int)tglp->cellWidth;
    int cellH = (int)tglp->cellHeight;
    int sheetW = (int)tglp->sheetWidth;
    if (cellW <= 0 || cellH <= 0 || sheetW < 8) return false;

    int perSheet = (int)tglp->nRows * (int)tglp->nLines;
    if (perSheet <= 0) return false;

    int sheet   = gi / perSheet;
    int inSheet = gi % perSheet;
    if (sheet < 0 || sheet >= (int)tglp->nSheets) return false;

    int lineId = inSheet / (int)tglp->nRows;
    int rowId  = inSheet % (int)tglp->nRows;

    // 官方几何：字形 cell 在 sheet 中的位置
    int cellX = rowId  * (cellW + 1) + 1;
    int cellY = lineId * (cellH + 1) + 1;

    const uint8 *sheetData = (const uint8 *)fontGetGlyphSheetTex(s_font, sheet);
    if (sheetData == NULL) return false;

    // ---- 取字形几何 ----
    // 直接扫描 cell 求出墨迹包围盒，不依赖 CWDH 的 xOffset / width
    // （不同字体对这两个字段的语义有细微差别，用实际像素最稳妥）。
    fontGlyphPos_s pos;
    memset(&pos, 0, sizeof(pos));
    fontCalcGlyphPos(&pos, s_font, gi, 0, 1.0f, 1.0f);

    int mx0 = cellW, my0 = cellH, mx1 = -1, my1 = -1;
    for (int y = 0; y < cellH; y++)
    {
        for (int x = 0; x < cellW; x++)
        {
            if (glyphAlpha(sheetData, sheetW, cellX + x, cellY + y) > 0)
            {
                if (x < mx0) mx0 = x;
                if (x > mx1) mx1 = x;
                if (y < my0) my0 = y;
                if (y > my1) my1 = y;
            }
        }
    }

    // advance 始终有效，即使字形是空白（空格等只前进、不绘制）
    int advance = (int)(pos.xAdvance * s_scale + 0.5f);
    if (advance <= 0 && pos.width > 0.5f)
        advance = (int)(pos.width * s_scale + 0.5f);
    if (advance <= 0)
        advance = (int)((float)cellW * s_scale + 0.5f);
    if (advance < 1) advance = 1;
    if (advance > 255) advance = 255;

    ent->advance = (uint8)advance;

    if (mx1 < 0 || my1 < 0)
    {
        // 空白字形（空格、零宽字符等）：不产生位图，只前进
        ent->w = 0;
        ent->h = 0;
        ent->offX = 0;
        ent->offY = 0;
        return true;
    }

    int srcW = mx1 - mx0 + 1;
    int srcH = my1 - my0 + 1;

    int dstW = (int)((float)srcW * s_scale + 0.5f);
    int dstH = (int)((float)srcH * s_scale + 0.5f);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;
    if (dstW > 39) dstW = 39;       // 与 GLYPH_MAX_PIXELS 匹配
    if (dstH > 39) dstH = 39;

    scaleGlyphArea(sheetData, sheetW,
                   cellX + mx0, cellY + my0, srcW, srcH,
                   dst, dstW, dstH);

    ent->w    = (uint8)dstW;
    ent->h    = (uint8)dstH;
    ent->offX = (int8)((int)((float)mx0 * s_scale + 0.5f));
    ent->offY = (int8)((int)((float)my0 * s_scale + 0.5f) + GLYPH_OFFY_ADJUST);

    return true;
}

//-----------------------------------------------------------------------
// 缓存
//-----------------------------------------------------------------------
static inline uint32 glyphHash(uint32 cp)
{
    return (cp * 2654435761u) & (uint32)GLYPH_CACHE_MASK;
}

static GlyphCacheEntry *cacheFind(uint32 cp)
{
    if (s_cache == NULL || cp == 0) return NULL;

    uint32 h = glyphHash(cp);
    for (int i = 0; i < GLYPH_PROBE_LIMIT; i++)
    {
        GlyphCacheEntry *e = &s_cache[(h + i) & (uint32)GLYPH_CACHE_MASK];
        if (e->codePoint == cp) return e;
        if (e->codePoint == 0)  return NULL;
    }
    return NULL;
}

static GlyphCacheEntry *cacheInsert(uint32 cp)
{
    if (s_cache == NULL || cp == 0) return NULL;

    uint32 h = glyphHash(cp);
    for (int i = 0; i < GLYPH_PROBE_LIMIT; i++)
    {
        GlyphCacheEntry *e = &s_cache[(h + i) & (uint32)GLYPH_CACHE_MASK];
        if (e->codePoint == 0 || e->codePoint == cp)
            return e;
    }
    return NULL;   // 探测链过长，放弃缓存（走直通）
}

static void noteMissing(uint32 cp)
{
    s_missing++;
    if (s_missingCount < MAX_MISSING)
        s_missingList[s_missingCount++] = cp;
}

//-----------------------------------------------------------------------
// 对外 API
//-----------------------------------------------------------------------
void ui3dsGlyphSetTargetHeight(int px)
{
    s_targetH = px;
    s_scale   = 1.0f;
    if (s_font != NULL && px > 0 && s_font->finf.tglp != NULL &&
        s_font->finf.tglp->cellHeight > 0)
    {
        s_scale = (float)px / (float)s_font->finf.tglp->cellHeight;
    }
}

int ui3dsGlyphGetTargetHeight(void)
{
    return s_targetH;
}

bool ui3dsGlyphInit(void)
{
    if (s_ready) return true;

    s_initTried = true;

    Result rc = fontEnsureMapped();
    if (R_FAILED(rc))
        return false;

    CFNT_s *font = fontGetSystemFont();
    if (font == NULL)
        return false;

    const TGLP_s *tglp = font->finf.tglp;
    if (tglp == NULL || tglp->sheetData == NULL)
        return false;
    if (tglp->cellWidth == 0 || tglp->cellHeight == 0 || tglp->nSheets == 0)
        return false;
    if (tglp->sheetWidth < 8 || tglp->sheetHeight < 8)
        return false;

    s_font     = font;
    s_sheetFmt = tglp->sheetFmt;

    if (s_cache == NULL)
        s_cache = (GlyphCacheEntry *)calloc(GLYPH_CACHE_SLOTS, sizeof(GlyphCacheEntry));
    if (s_pool == NULL)
        s_pool = (uint8 *)malloc(GLYPH_POOL_BYTES);

    if (s_cache == NULL)
        return false;   // 没有缓存表就无法安全运行

    s_poolUsed = 0;

    // 目标高度为 0 时使用系统原生高度
    int t = (s_targetH > 0) ? s_targetH : (int)tglp->cellHeight;
    s_targetH = t;
    s_scale   = (float)t / (float)tglp->cellHeight;

    s_ready = true;
    return true;
}

bool ui3dsGlyphAvailable(void)
{
    if (!s_ready && !s_initTried)
        ui3dsGlyphInit();       // 延迟初始化：首次查询时自动尝试

    return s_ready;
}

bool ui3dsGlyphHas(uint32 codePoint)
{
    if (!s_ready) return false;

    int gi = fontGlyphIndexFromCodePoint(s_font, codePoint);
    if (gi < 0) return false;
    if (gi == (int)s_font->finf.alterCharIndex) return false;
    return true;
}

const uint8 *ui3dsGlyphGet(uint32 codePoint,
                           int *outW, int *outH,
                           int *outOffX, int *outOffY,
                           int *outAdvance)
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (outOffX) *outOffX = 0;
    if (outOffY) *outOffY = 0;
    if (outAdvance) *outAdvance = 0;

    if (!s_ready) return NULL;

    // U+0000 不参与缓存（0 是空槽标记）
    if (codePoint == 0)
    {
        if (outW) *outW = 0;
        if (outH) *outH = 0;
        return (const uint8 *)s_scratch;
    }

    GlyphCacheEntry *e = cacheFind(codePoint);
    if (e != NULL)
    {
        s_hits++;
        if (outW) *outW = e->w;
        if (outH) *outH = e->h;
        if (outOffX) *outOffX = e->offX;
        if (outOffY) *outOffY = e->offY;
        if (outAdvance) *outAdvance = e->advance;
        return (e->w > 0) ? (const uint8 *)(s_pool + e->offset) : (const uint8 *)s_scratch;
    }

    // 未命中：先构建到临时缓冲
    GlyphCacheEntry tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.codePoint = codePoint;

    if (!buildGlyph(codePoint, &tmp, s_scratch))
    {
        noteMissing(codePoint);
        return NULL;
    }

    s_misses++;

    // 尝试写入缓存池
    uint32 need = (uint32)tmp.w * (uint32)tmp.h;
    GlyphCacheEntry *slot = cacheInsert(codePoint);

    if (slot != NULL && s_pool != NULL && (s_poolUsed + need) <= (uint32)GLYPH_POOL_BYTES)
    {
        uint32 off = s_poolUsed;
        if (need > 0)
            memcpy(s_pool + off, s_scratch, need);
        s_poolUsed += need;

        *slot = tmp;
        slot->offset = off;

        e = slot;
    }
    else
    {
        // 池满或探测链过长 —— 直通渲染，结果与命中缓存完全一致
        e = &tmp;
    }

    if (outW) *outW = e->w;
    if (outH) *outH = e->h;
    if (outOffX) *outOffX = e->offX;
    if (outOffY) *outOffY = e->offY;
    if (outAdvance) *outAdvance = e->advance;

    if (e->w > 0)
    {
        if (e == &tmp)
            return (const uint8 *)s_scratch;      // 直通
        return (const uint8 *)(s_pool + e->offset); // 已入池
    }
    return (const uint8 *)s_scratch;
}

int ui3dsGlyphAdvance(uint32 codePoint)
{
    int w = 0, h = 0, ox = 0, oy = 0, adv = 0;
    const uint8 *bmp = ui3dsGlyphGet(codePoint, &w, &h, &ox, &oy, &adv);
    if (bmp == NULL)
        return 0;
    return adv;
}

void ui3dsGlyphGetStats(int *hits, int *misses, int *cachedCount, int *missingCount)
{
    if (hits)   *hits   = s_hits;
    if (misses) *misses = s_misses;
    if (missingCount) *missingCount = s_missing;

    if (cachedCount)
    {
        int n = 0;
        if (s_cache != NULL)
        {
            for (int i = 0; i < GLYPH_CACHE_SLOTS; i++)
                if (s_cache[i].codePoint != 0) n++;
        }
        *cachedCount = n;
    }
}

int ui3dsGlyphDumpMissing(uint32 *outBuf, int maxCount)
{
    int n = (s_missingCount < maxCount) ? s_missingCount : maxCount;
    for (int i = 0; i < n; i++)
        outBuf[i] = s_missingList[i];
    return n;
}
