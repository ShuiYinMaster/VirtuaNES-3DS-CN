
#ifndef _3DSFONTPROVIDER_H_
#define _3DSFONTPROVIDER_H_

#include "3dstypes.h"

//-----------------------------------------------------------------------
// 字形提供者（Glyph Provider）
//
// 数据源：3DS 系统共享字体（libctru <3ds/font.h>）。
// 用途：给 3dsui.cpp 提供"按 Unicode 码位取字形位图"的能力，替代原先
//       只能处理单字节的 fontBitmap[c * 256 + ...] 查表 —— 后者正是
//       中文乱码的根源（UTF-8 的三个字节被当成三个西欧字形来画）。
//
// 设计要点：
//   * 字形来源只有系统字体，不依赖任何外部字库文件，零体积增量。
//   * 缓存只增不减（永不淘汰）。缓存池耗尽后自动切换到"直通渲染"
//     （实时采样、不写缓存），显示结果与命中缓存时完全一致 —— 也就
//     是说，缓存容量永远不会影响显示正确性，不会出现"字多了就缺字"。
//   * 系统字体中确实不存在的码位返回 NULL 并记入缺字表，由调用方
//     绘制缺字方框，便于事后定位补字。
//-----------------------------------------------------------------------

// 位图 alpha 值域上限。与 3dsui.cpp 的 MAX_ALPHA 保持一致（3 bit）。
#define UI3DS_GLYPH_ALPHA_MAX   8

//-----------------------------------------------------------------------
// 目标字形 cell 高度（像素）。等于缩放后的一行高度。
// 传入 0 表示使用系统字体原生 cellHeight（本机实测为 24）。
//-----------------------------------------------------------------------
void ui3dsGlyphSetTargetHeight(int px);
int  ui3dsGlyphGetTargetHeight(void);

//-----------------------------------------------------------------------
// 初始化：映射系统共享字体并分配字形缓存。
// 返回 true  表示多字节（中文）渲染可用。
// 返回 false 时调用方应退回使用原来的单字节点阵字体。
//-----------------------------------------------------------------------
bool ui3dsGlyphInit(void);

// 字体是否可用（可在初始化后随时查询）
bool ui3dsGlyphAvailable(void);

// 该码位是否有字形（false = 缺字，调用方应画方框而非乱码）
bool ui3dsGlyphHas(uint32 codePoint);

//-----------------------------------------------------------------------
// 取字形位图。
//
// 成功时返回位图首地址（每行 *outW 字节，像素取值 0..UI3DS_GLYPH_ALPHA_MAX），
// 并填写：
//   *outOffX / *outOffY  —— 相对笔位的绘制偏移（像素）
//   *outAdvance          —— 画完后笔位应前进的距离（像素）
//
// 返回 NULL 表示缺字。若字形本身为空白（如空格），返回非 NULL 且 *outW == 0，
// 调用方只需按 *outAdvance 前进、不做绘制。
//
// 注意：返回的指针仅在"下次调用本函数之前"有效 —— 缓存命中时指向常驻字形池，
//       未命中时指向内部临时缓冲。请勿跨字符持有。
//-----------------------------------------------------------------------
const uint8 *ui3dsGlyphGet(uint32 codePoint,
                           int *outW, int *outH,
                           int *outOffX, int *outOffY,
                           int *outAdvance);

// 只取笔位步进宽度（像素）。缺字返回缺字方框的宽度。
int ui3dsGlyphAdvance(uint32 codePoint);

// 统计信息（调试用，任一参数可为 NULL）
void ui3dsGlyphGetStats(int *hits, int *misses, int *cachedCount, int *missingCount);

// 导出缺字码位（返回实际写入个数），用于事后核对字库覆盖
int ui3dsGlyphDumpMissing(uint32 *outBuf, int maxCount);

#endif
