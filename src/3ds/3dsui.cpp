//=============================================================================
// Basic user interface framework for low-level drawing operations to
// the bottom screen.
//=============================================================================

#include <cstdio>
#include <cstring>

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <3ds.h>

#include "3dsinterface.h"
#include "3dstypes.h"
#include "3dsui.h"
#include "3dsfont.h"
#include "3dsgbk.h"
#include "3dsutf8togbk.h"
#include "3dsfontprovider.h"

int foreColor = 0xffffff;
int backColor = 0x000000;

int translateX = 0;
int translateY = 0;
int viewportX1 = 0;
int viewportY1 = 0;
int viewportX2 = 320;
int viewportY2 = 240;

//=============================================================================
// 排版参数（中西文混排"美化"的核心）
//=============================================================================

// 菜单/列表行距（像素）。
//
// 【布局硬约束 —— 不要越过】
// 底屏的垂直划分（3dsmenu.cpp: menu3dsDrawMenu）：
//     0..24    顶栏（标签页）
//     24..220  列表内容区
//     220..240 页脚状态栏（menuBottomBarColor + 提示文字画在 y=223）
// 列表首行起点 menuStartY = 29，可见行数由 menu3dsGetMaxVisibleItems() 反推。
// 必须满足：29 + 行数 * 本值 <= 220。
//
// 这个约束以前是"隐式靠余量成立"的，极其脆弱：
//   行距 13： 29 + 14*13 = 211  ->  余 9px   正常
//   行距 15： 29 + 14*15 = 239  ->  越界 19px，最后两行直接压在页脚上
// 我上一轮就是只算了"29 + n*13 离 240 还差多少"，把页脚起点误当成 240，
// 于是得出"底部白扔 29px"的错误结论，把行距提到 15 —— 结果侵入页脚。
// 现在可见行数改为自动反推（见 3dsmenu.cpp），改本值最多是"行数变少"，
// 不会再越界。
//
// 注意：内置 GBK 点阵的字形尺寸与该值无关 —— 绘制固定取 cell 第 1..13 行，
// 汉字墨迹恒为行框第 1..12 行，上下都不裁切，且始终 1:1 不缩放（锐利的来源）。
// 所以本值只影响行距，不影响汉字大小与清晰度。
#define UI3DS_FONT_HEIGHT   13

// 对话框内"自动换行文本"的行距（像素）。
//
// 必须与菜单行距解耦：对话框描述文本被限制在固定坐标框内
// （3dsmenu.cpp: ui3dsDrawStringWithWrapping(30, 30, 290, 70)，高 40px），
// 且下一段内容从 y=80 开始。行距 14 会让第三行的最后一行像素顶到框底被裁掉；
// 13 时三行都留有余量（30/43/56，墨迹到 68）。同时它比原版的 12 多 1px，
// 中文不再行行相贴。
#define UI3DS_WRAPPED_LINE_PITCH   13

// 中西文交界处的自动间距（像素）——即"自动调整中文与西文的间距"这条排版规则。
//
// 汉字是 12px 满格方块，西文字母只有 2..9px 宽，两者直接相邻会挤成一团。
// 实测对照（probe_out/typo_sheet_E.png）：
//   0px = 明显贴住   1px = 恰好   2px = 偏松   3px(1/4 em) = 像插了个空格
// 只在"中日韩宽字符"与"西文"直接相邻时插入；中间本来就有空格时不再叠加，
// 避免出现双倍空隙。
#define UI3DS_SCRIPT_GAP    1

// CJK 字形来源优先级：
//   1 = 内置 GBK 点阵优先（12px 原生双色点阵，边缘锐利，与英文点阵同尺寸）
//   0 = 系统共享字体优先（覆盖最全，但边缘偏柔）
// 无论取哪个，另一条都会作为后备，绝不出现乱码或空白。
#define UI3DS_PREFER_BITMAP_FONT   1

// 汉字步进（像素）。实测该字库字形墨迹为 cell 第 0..10 列，取 12 让相邻汉字之间
// 留 1px 间隙 —— 与参考实现（R-YaTian 分支）一致。
// 这个值同时是缺字方框的边长：两者必须一致，否则测宽与绘制会对不上。
#define UI3DS_CJK_CELL   12

int fontHeight = UI3DS_FONT_HEIGHT;

// 当前西文字体索引（0=Tempesta 1=Ronda 2=Arial），与 fontBitmap/fontWidth 同步。
static int ui3dsCurrentFontIndex = 0;

// 三套西文字体的纵向补偿（像素）。
//
// 实测（_scratch/font_metrics3.py）：Tempesta 与 Ronda 的大写墨迹在 cell 第 4..10 行，
// Arial 在 3..9 行 —— 也就是 Arial 天生高一格。若不补偿，用户把字体切到 Arial 时，
// 汉字相对西文会整体"下沉"1px，破坏中西文的基线关系。这里把 Arial 下移 1px，
// 让三套字体与汉字的基线关系完全一致：切换字体时中西文相对位置不变。
static const int ui3dsLatinOffY[3] = { 0, 0, 1 };

int viewportStackCount = 0;
int viewportStack[20][4];

// gbk things
const int gbkRowStart = 0xa1;
const int gbkRowEnd = 0xf7;
const int gbkColStart = 0xa1;
const int gbkColEnd = 0xfe;

#define MAX_ALPHA 8
typedef struct
{
    int red[MAX_ALPHA + 1][32];
    int green[MAX_ALPHA + 1][32];
    int blue[MAX_ALPHA + 1][32];
} SAlpha;

SAlpha alphas;

uint8 *fontWidthArray[] = { fontTempestaWidth, fontRondaWidth, fontArialWidth };
uint8 *fontBitmapArray[] = { fontTempestaBitmap, fontRondaBitmap, fontArialBitmap };

uint8 *fontBitmap;
uint8 *fontWidth;

//---------------------------------------------------------------
// Initialize this library
//---------------------------------------------------------------
void ui3dsInitialize()
{
    // Precompute alphas
    //
    for (int i = 0; i < 32; i++)
    {
        for (int a = 0; a <= MAX_ALPHA; a++)
        {
            int f = i * a / MAX_ALPHA;
            alphas.red[a][i] = f << 11;
            alphas.green[a][i] = f << 6;
            alphas.blue[a][i] = f;
        }
    }

    // Initialize fonts
    //
    for (int f = 0; f < 3; f++)
    {
        fontBitmap = fontBitmapArray[f];
        fontWidth = fontWidthArray[f];
        for (int i = 0; i < 65536; i++)
        {
            uint8 c = fontBitmap[i];
            if (c == ' ')
                fontBitmap[i] = 0;
            else
                fontBitmap[i] = c - '0';
        }

        fontWidth[1] = 10;
        fontWidth[10] = 0;
        fontWidth[13] = 0;
        fontWidth[248] = 10;
        fontWidth[249] = 10;
        fontWidth[250] = 9;
        fontWidth[251] = 1;
        fontWidth[253] = 10;
        fontWidth[254] = 10;
        fontWidth[255] = 7;
    }

    fontBitmap = fontTempestaBitmap;
    fontWidth = fontTempestaWidth;

    viewportX1 = 0;
    viewportY1 = 0;
    viewportX2 = 320;
    viewportY2 = 240;

    // 初始化 3DS 系统共享字体提供者（含 CJK 字形，零字库文件）。
    // 失败也不会中断：中文会退回外部 GBK 点阵字库，拉丁文本始终走原点阵字体。
    ui3dsGlyphSetTargetHeight(UI3DS_FONT_HEIGHT);
    ui3dsGlyphInit();
}

//---------------------------------------------------------------
// Get the target screen width
//---------------------------------------------------------------
int ui3dsGetScreenWidth(gfxScreen_t targetScreen) {
    return (targetScreen == GFX_TOP) ? SCREEN_TOP_WIDTH : SCREEN_BOTTOM_WIDTH;
}


//---------------------------------------------------------------
// Returns the global text line height in pixels.
//---------------------------------------------------------------
int ui3dsGetFontHeight()
{
    return fontHeight;
}

//---------------------------------------------------------------
// this is called on init and if game screen has swapped
//---------------------------------------------------------------
void ui3dsUpdateScreenSettings(gfxScreen_t gameScreen) {
    screenSettings.GameScreen = gameScreen;
    screenSettings.SecondScreen = (screenSettings.GameScreen == GFX_TOP) ? GFX_BOTTOM : GFX_TOP;
    screenSettings.GameScreenWidth  = ui3dsGetScreenWidth(screenSettings.GameScreen);
    screenSettings.SecondScreenWidth  = ui3dsGetScreenWidth(screenSettings.SecondScreen);

    // reset menu viewport
    viewportX1 = 0;
    viewportY1 = 0;
    viewportX2 = screenSettings.SecondScreenWidth;
    viewportY2 = 240;
    viewportStackCount = 0;
}

//---------------------------------------------------------------
// Sets the font to be used to display text to the screen.
//---------------------------------------------------------------
void ui3dsSetFont(int fontIndex, bool init)
{
    if (fontIndex >= 0 && fontIndex < 3)
    {
        fontBitmap = fontBitmapArray[fontIndex];
        fontWidth = fontWidthArray[fontIndex];
        ui3dsCurrentFontIndex = fontIndex;      // 供中西文基线补偿查表
        if (!init)
        {
            settings3DS.Font = fontIndex;
            impl3dsReadWriteSettingsGlobal(true);
        }
    }
}

//---------------------------------------------------------------
// Sets the menu theme.
//---------------------------------------------------------------
void ui3dsSetTheme(int themeIndex)
{
    if (themeIndex >= 0 && themeIndex < 2)
    {
        settings3DS.Theme = themeIndex;
        impl3dsReadWriteSettingsGlobal(true);
    }
}

//---------------------------------------------------------------
// Sets the ui language.
//---------------------------------------------------------------
void ui3dsSetLanguage(int langIndex, bool init)
{
    if (!init)
    {
        if (langIndex >= 0 && langIndex < 2)
        {
            settings3DS.Language = langIndex;
            impl3dsReadWriteSettingsGlobal(true);
        }
    }

    switch(langIndex)
    {
        case 1:
            setLanguage("en_US");
            break;
        default:
            setLanguage("zh_Hans");
            break;
    }
}

//---------------------------------------------------------------
// Sets the global viewport for all drawing
//---------------------------------------------------------------
void ui3dsSetViewport(int x1, int y1, int x2, int y2)
{
    viewportX1 = x1;
    viewportX2 = x2;
    viewportY1 = y1;
    viewportY2 = y2;

    if (viewportX1 < 0) viewportX1 = 0;
    if (viewportX2 > screenSettings.SecondScreenWidth) viewportX2 = screenSettings.SecondScreenWidth;
    if (viewportY1 < 0) viewportY1 = 0;
    if (viewportY2 > 240) viewportY2 = 240;
}

//---------------------------------------------------------------
// Push the global viewport for all drawing
//---------------------------------------------------------------
void ui3dsPushViewport(int x1, int y1, int x2, int y2)
{
    if (viewportStackCount < 10)
    {
        if (x1 < viewportX1) x1 = viewportX1;
        if (x2 > viewportX2) x2 = viewportX2;
        if (y1 < viewportY1) y1 = viewportY1;
        if (y2 > viewportY2) y2 = viewportY2;

        viewportStack[viewportStackCount][0] = viewportX1;
        viewportStack[viewportStackCount][1] = viewportX2;
        viewportStack[viewportStackCount][2] = viewportY1;
        viewportStack[viewportStackCount][3] = viewportY2;
        viewportStackCount++;

        ui3dsSetViewport(x1, y1, x2, y2);
    }
}

//---------------------------------------------------------------
// Pop the global viewport
//---------------------------------------------------------------
void ui3dsPopViewport()
{
    if (viewportStackCount > 0)
    {
        viewportStackCount--;
        viewportX1 = viewportStack[viewportStackCount][0];
        viewportX2 = viewportStack[viewportStackCount][1];
        viewportY1 = viewportStack[viewportStackCount][2];
        viewportY2 = viewportStack[viewportStackCount][3];
    }
}


//---------------------------------------------------------------
// Applies alpha to a given colour.
// NOTE: Alpha is a value from 0 to 10. (0 = transparent, 10 = opaque)
//---------------------------------------------------------------
inline uint16 __attribute__((always_inline)) ui3dsApplyAlphaToColour565(int color565, int alpha)
{
    int red = (color565 >> 11) & 0x1f;
    int green = (color565 >> 6) & 0x1f; // drop the LSB of the green colour
    int blue = (color565) & 0x1f;

    int result = alphas.red[alpha][red] | alphas.blue[alpha][blue] | alphas.green[alpha][green];
    //printf ("result %x * %d = (%x | %x | %x) = %x\n", color565, alpha,
    //   alphas.red[alpha][red], alphas.green[alpha][green], alphas.blue[alpha][blue], result);
    return result;
}


//---------------------------------------------------------------
// Multiplies the alpha to the color in RGBA8 format,
// and returns the results.
//---------------------------------------------------------------
int ui3dsApplyAlphaToColor(int color, float alpha)
{
    if (alpha < 0)      alpha = 0;
    if (alpha > 1.0f)   alpha = 1.0f;

    int a = (int)(alpha * 255);

    return
        ((((color >> 16) & 0xff) * a / 255) << 16) |
        ((((color >> 8) & 0xff) * a / 255) << 8) |
        ((((color >> 0) & 0xff) * a / 255) << 0);
}


// overlay blending mode: returns a color in RGB888 format
// may have more performance impact than simple blending mode 
// but will provide more vibrant colors
// TODO: add alpha value support
int ui3dsOverlayBlendColor(int backgroundColor, int foregroundColor) {
    // Extract the red, green, and blue components of the colors
    float baseR = ((backgroundColor >> 16) & 0xFF) / 255.0f;
    float baseG = ((backgroundColor >> 8) & 0xFF) / 255.0f;
    float baseB = (backgroundColor & 0xFF) / 255.0f;
    
    float blendR = ((foregroundColor >> 16) & 0xFF) / 255.0f;
    float blendG = ((foregroundColor >> 8) & 0xFF) / 255.0f;
    float blendB = (foregroundColor & 0xFF) / 255.0f;

    float resultR = baseR <= 0.5f ? 2 * baseR * blendR : 1 - 2 * (1 - baseR) * (1 - blendR);
    float resultG = baseG <= 0.5f ? 2 * baseG * blendG : 1 - 2 * (1 - baseG) * (1 - blendG);
    float resultB = baseB <= 0.5f ? 2 * baseB * blendB : 1 - 2 * (1 - baseB) * (1 - blendB);

    unsigned char r = static_cast<unsigned int>(resultR * 255);
    unsigned char g = static_cast<unsigned int>(resultG * 255);
    unsigned char b = static_cast<unsigned int>(resultB * 255);

    return (r << 16) | (g << 8) | b;
}


//---------------------------------------------------------------
// Sets the global translate for all drawing
//---------------------------------------------------------------
void ui3dsSetTranslate(int tx, int ty)
{
    translateX = tx;
    translateY = ty;
}


//---------------------------------------------------------------
// Computes the frame buffer offset given the x, y
// coordinates.
//---------------------------------------------------------------
inline int __attribute__((always_inline)) ui3dsComputeFrameBufferOffset(int x, int y)
{
    return ((x) * 240 + (239 - y));
}


//---------------------------------------------------------------
// Gets a pixel colour.
//---------------------------------------------------------------
inline uint16 __attribute__((always_inline)) ui3dsGetPixelInline(uint16 *frameBuffer, int x, int y)
{
    return frameBuffer[ui3dsComputeFrameBufferOffset((x), (y))];
}


//---------------------------------------------------------------
// Sets a pixel colour.
//---------------------------------------------------------------
inline void __attribute__((always_inline)) ui3dsSetPixelInline(uint16 *frameBuffer, int x, int y, int color)
{
    if (color < 0) return;
    if ((x) >= viewportX1 && (x) < viewportX2 &&
        (y) >= viewportY1 && (y) < viewportY2)
    {
        frameBuffer[ui3dsComputeFrameBufferOffset((x), (y))] = color;
    }
}


//---------------------------------------------------------------
// Draws a single character to the screen
//---------------------------------------------------------------
void ui3dsDrawChar(uint16 *frameBuffer, int x, int y, int color565, uint8 c)
{
    // Draws a character to the screen at (x,y)
    // (0,0) is at the top left of the screen.
    //
    int wid = fontWidth[c];
    uint8 alpha;
    //printf ("d %c (%d)\n", c, bmofs);

    if ((y) >= viewportY1 && (y) < viewportY2)
    {
        for (int x1 = 0; x1 < wid; x1++)
        {
            #define GETFONTBITMAP(c, x, y) fontBitmap[c * 256 + x + (y)*16]

            #define SETPIXELFROMBITMAP(y1) \
                alpha = GETFONTBITMAP(c,x1,y1); \
                ui3dsSetPixelInline(frameBuffer, cx, cy + y1, \
                alpha == MAX_ALPHA ? color565 : \
                alpha == 0x0 ? -1 : \
                    ui3dsApplyAlphaToColour565(color565, alpha) + \
                    ui3dsApplyAlphaToColour565(ui3dsGetPixelInline(frameBuffer, cx, cy + y1), MAX_ALPHA - alpha));

            int cx = (x + x1);
            // 西文纵向补偿：三套字体的墨迹行位不同（Arial 天生高一格），
            // 下移补偿后各字体与汉字的基线关系保持一致。
            int cy = (y) + ui3dsLatinOffY[ui3dsCurrentFontIndex];
            if (cx >= viewportX1 && cx < viewportX2)
            {
                for (int h = 0; h < fontHeight; h++)
                {
                    SETPIXELFROMBITMAP(h);
                }
            }
        }
    }
}


//---------------------------------------------------------------
// Draws a gbk character to the screen
//---------------------------------------------------------------
void ui3dsDrawGBK(uint16 *frameBuffer, int x, int y, int color565, uint16 ch) {
    const int rowCount = (gbkRowEnd - gbkRowStart + 1);
    const int colCount = (gbkColEnd - gbkColStart + 1);

    int r = ch >> 8;
    int c = ch & 0xff;

    if(fontGBK == NULL
        || r < gbkRowStart || r > gbkRowEnd
        || c < gbkColStart || c > gbkColEnd){
        ui3dsDrawChar(frameBuffer, x, y, color565, '?');
        return;
    }

    // Draws a character to the screen at (x,y)
    // (0,0) is at the top left of the screen.
    int wid = 12;
    uint8 alpha;

    int idx = (r - gbkRowStart) * colCount + (c - gbkColStart);

    if ((y) >= viewportY1 && (y) < viewportY2)
    {
        for (int x1 = 0; x1 < wid; x1++)
        {
            #define GETFONTBITMAP_GBK(c, x, y) fontGBK[c * 256 + x + (y+1)*16]

            #define SETPIXELFROMBITMAP_GBK(y1) \
                alpha = GETFONTBITMAP_GBK(idx,x1,y1); \
                ui3dsSetPixelInline(frameBuffer, cx, cy + y1, \
                alpha == MAX_ALPHA ? color565 : \
                alpha == 0x0 ? -1 : \
                    ui3dsApplyAlphaToColour565(color565, alpha) + \
                    ui3dsApplyAlphaToColour565(ui3dsGetPixelInline(frameBuffer, cx, cy + y1), MAX_ALPHA - alpha));

            int cx = (x + x1);
            int cy = (y);
            if (cx >= viewportX1 && cx < viewportX2)
            {
                for (int h = 0; h < fontHeight; h++)
                {
                    SETPIXELFROMBITMAP_GBK(h);
                }
            }
        }
    }
}


//=============================================================================
// 多字节（UTF-8）文本支持 —— 内置 GBK 点阵优先，系统共享字体兜底
//
// 原实现把每个字节直接当成字形索引，而中文 UTF-8 的三个字节会落进
// 128..255 的西欧重音字符槽位，于是显示成 Ã¤Â½Â* 一类乱码。
// 下面这组函数把渲染改成"按 Unicode 码位"，字形来源依次为：
//   1) 内置 GBK 点阵（gbk.bin，随 romfs 分发）—— 12px 原生双色点阵，
//      零抗锯齿，边缘锐利，尺寸与英文点阵齐平。覆盖 GB2312 全部汉字
//      （6763 个常用字 + 符号区），即中文文件名/菜单的实际全部用字。
//   2) 3DS 系统共享字体 —— 覆盖 GB2312 之外的一切（生僻字、繁体、假名、
//      韩文、表情符号等）。由 24px 轮廓字缩放而来，边缘偏柔，故只作兜底。
//   3) 都没有 -> 画缺字方框，而不是输出乱码或空白。
//
// 两条来源的取舍由 UI3DS_PREFER_BITMAP_FONT 控制，改一个数字即可互换。
//=============================================================================

// 旧式单字节字形码位哨兵。
//
// emus3ds 的 UI 图标是**单字节字体索引**，直接写进字符串字面量：
//   0xF8 上箭头 / 0xF9 下箭头 / 0xFA..0xFC 进度条 / 0xFD 复选框"勾选" /
//   0xFE 复选框"未勾选"     （见 3dsmenu.cpp: menu3dsDrawItems）
// 这些字节（0xF8..0xFF）以及 0x80..0xBF 都**不是合法的 UTF-8 首字节**。
//
// 改造前它们走"单字节直查字形表"，正常显示；改造后若一律当缺字，
// 复选框/箭头/进度条就会全部变成空心方框。
// 所以把"不构成合法 UTF-8 序列的单字节"标成这个哨兵值（超出 Unicode 上限
// 0x10FFFF，不可能与真实码位冲突），让测宽与绘制回落到内置字体单字节点阵。
#define UI3DS_LEGACY_BYTE_BASE   0x110000u

// UTF-8 解码。返回码位，*len 为消耗字节数（1..4）。
// 非法序列返回 UI3DS_LEGACY_BYTE_BASE + 字节值（= 旧式单字节字形索引）。
static uint32 ui3dsUtf8Decode(const char *s, int pos, int *len)
{
    const uint8 *p = (const uint8 *)(s + pos);
    uint8 b0 = p[0];
    *len = 1;

    if (b0 < 0x80)
        return (uint32)b0;

    if ((b0 & 0xE0) == 0xC0 && p[1] != 0 && (p[1] & 0xC0) == 0x80)
    {
        *len = 2;
        return ((uint32)(b0 & 0x1F) << 6) | (uint32)(p[1] & 0x3F);
    }

    if ((b0 & 0xF0) == 0xE0 && p[1] != 0 && p[2] != 0 &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        *len = 3;
        return ((uint32)(b0 & 0x0F) << 12) |
               ((uint32)(p[1] & 0x3F) << 6) |
                (uint32)(p[2] & 0x3F);
    }

    if ((b0 & 0xF8) == 0xF0 && p[1] != 0 && p[2] != 0 && p[3] != 0 &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        *len = 4;
        return ((uint32)(b0 & 0x07) << 18) |
               ((uint32)(p[1] & 0x3F) << 12) |
               ((uint32)(p[2] & 0x3F) << 6) |
                (uint32)(p[3] & 0x3F);
    }

    // 不是合法 UTF-8 序列 —— 按"旧式单字节字形索引"处理（见上方哨兵说明）。
    // 这里**不能**返回 U+FFFD：那会把 UI 图标（复选框/箭头/进度条）全部变成缺字方框。
    *len = 1;
    return UI3DS_LEGACY_BYTE_BASE + (uint32)b0;
}

// 外部 GBK 点阵中该码位是否有字形；*outGbk 回传对应的 GBK 双字节码
static bool ui3dsGbkHasGlyph(uint32 codePoint, uint16 *outGbk)
{
    if (fontGBK == NULL || codePoint > 0xFFFF)
        return false;

    uint16 chGBK = getGbkChar((uint16)codePoint);
    uint8 cg1 = (chGBK & 0xFF00) >> 8;
    uint8 cg2 = chGBK & 0x00FF;

    if (cg1 >= gbkRowStart && cg1 <= gbkRowEnd &&
        cg2 >= gbkColStart && cg2 <= gbkColEnd)
    {
        // GB2312 区里有 688 个未定义槽位（整槽全 0）。必须把它们当作
        // "字库没有这个字"，交给系统共享字体兜底 —— 否则会画出空白，
        // 既看不出缺字、也拿不到兜底字形。
        int slot = (cg1 - gbkRowStart) * (gbkColEnd - gbkColStart + 1)
                 + (cg2 - gbkColStart);
        if (!gbk3dsSlotUsed(slot))
            return false;

        if (outGbk) *outGbk = chGBK;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------
// 中西文混排：字形类别 与 "自动间距"
//
// 这是让中西文"协调"的关键：汉字是满格方块，西文是窄字母，直接相邻会互相挤压。
// 传统排版里对应的规则叫"自动调整中文与西文的间距"（Word / InDesign 都内置）。
//-----------------------------------------------------------------------

// 类别：0 = 不参与（空格、字符串首尾），1 = 西文，2 = 中日韩宽字符
static inline int ui3dsScriptClass(uint32 cp)
{
    if (cp == 0) return 0;
    if (cp == ' ') return 0;            // 空格本身就是分隔符，不再叠加间距
    if (cp >= UI3DS_LEGACY_BYTE_BASE) return 1;   // 旧式单字节字形（UI 图标）算西文，不插间距
    if (cp < 0x80) return 1;
    // 与"三字节 UTF-8 即视作宽字符"的既有字形判据保持一致
    return (cp >= 0x800) ? 2 : 1;
}

// 相邻两字符之间应插入的额外间距
static inline int ui3dsScriptGap(uint32 prevCp, uint32 cp)
{
    int a = ui3dsScriptClass(prevCp);
    int b = ui3dsScriptClass(cp);
    if (a != 0 && b != 0 && a != b)
        return UI3DS_SCRIPT_GAP;
    return 0;
}

// 用内置 GBK 点阵测宽。命中返回 true 并写出宽度/字节数。
static inline bool ui3dsTryMeasureBitmap(uint32 cp, int len, int *outWidth, int *outLen)
{
    if (len != 3)                       // 只处理三字节区段（U+0800..U+FFFF）
        return false;
    if (!ui3dsGbkHasGlyph(cp, NULL))
        return false;

    *outWidth = UI3DS_CJK_CELL;
    *outLen   = 3;
    return true;
}

// 用 3DS 系统共享字体测宽。命中返回 true；缺字返回 false（留给下一级来源）。
static inline bool ui3dsTryMeasureSystem(uint32 cp, int len, int *outWidth, int *outLen)
{
    if (!ui3dsGlyphAvailable())
        return false;
    if (!ui3dsGlyphHas(cp))
        return false;

    int adv = ui3dsGlyphAdvance(cp);
    *outWidth = (adv > 0) ? adv : UI3DS_CJK_CELL;
    *outLen   = len;
    return true;
}

// 单个字符的显示宽度（像素，**含**它与前一字符之间的中西文间距）。
//
// 这是全局唯一的"一个字占多宽"的口径：测宽（居中/右对齐/换行）与绘制前进
// 都走它，因此三者不可能对不上。prevCp 为前一字符的码位（0 = 行首）。
static int ui3dsMeasureCharCJK(const char *s, int pos, int *outLen, uint32 prevCp = 0)
{
    uint8 c = (uint8)s[pos];
    *outLen = 1;
    if (c == 0) return 0;

    if (c < 0x80)
        return fontWidth[c] + ui3dsScriptGap(prevCp, (uint32)c);   // ASCII 仍走原单字节点阵

    int len = 1;
    uint32 cp = ui3dsUtf8Decode(s, pos, &len);
    if (len < 1) len = 1;

    // 旧式单字节字形（UI 图标 0xF8..0xFE 等）：与 ASCII 同一口径，
    // 直接取内置字体的单字节宽度。**必须**在 GBK/系统字体之前拦下，
    // 否则这些图标会以 U+FFFD 的姿态落到"缺字方框"分支。
    if (cp >= UI3DS_LEGACY_BYTE_BASE)
    {
        *outLen = 1;
        return fontWidth[c] + ui3dsScriptGap(prevCp, (uint32)c);
    }

    int gap = ui3dsScriptGap(prevCp, cp);
    int w = 0;

    if (UI3DS_PREFER_BITMAP_FONT)
    {
        // 1) 内置 GBK 点阵（锐利、与英文同尺寸）
        if (ui3dsTryMeasureBitmap(cp, len, &w, outLen)) return w + gap;
        // 2) 系统共享字体（覆盖一切）
        if (ui3dsTryMeasureSystem(cp, len, &w, outLen)) return w + gap;
    }
    else
    {
        if (ui3dsTryMeasureSystem(cp, len, &w, outLen)) return w + gap;
        if (ui3dsTryMeasureBitmap(cp, len, &w, outLen)) return w + gap;
    }

    // 3) 两级来源都没有：系统字体可用时画缺字方框，否则退回逐字节行为
    if (ui3dsGlyphAvailable())
    {
        *outLen = len;
        return UI3DS_CJK_CELL + gap;
    }

    *outLen = 1;
    return fontWidth[c] + gap;
}

// 绘制缺字方框（空心）。用方框而不是 '?'，便于一眼分辨"字体确实缺此字"。
//
// 尺寸固定为 UI3DS_CJK_CELL（= 汉字步进），**不能**跟着行高走：测宽函数对缺字
// 返回的也是这个值，两者必须一致，否则方框与后续文字会错位。
// 纵向对齐汉字墨迹（汉字墨迹在行框第 1..12 行）。
static void ui3dsDrawMissingGlyphBox(uint16 *frameBuffer, int x, int y, int color565)
{
    const int w = UI3DS_CJK_CELL;
    const int h = UI3DS_CJK_CELL;
    const int top = y + 1;

    for (int i = 0; i < w; i++)
    {
        ui3dsSetPixelInline(frameBuffer, x + i, top, color565);
        ui3dsSetPixelInline(frameBuffer, x + i, top + h - 1, color565);
    }
    for (int j = 0; j < h; j++)
    {
        ui3dsSetPixelInline(frameBuffer, x, top + j, color565);
        ui3dsSetPixelInline(frameBuffer, x + w - 1, top + j, color565);
    }
}

// 用 3DS 系统共享字体绘制一个 Unicode 码位，返回笔位前进量（像素）。
static int ui3dsDrawSystemGlyph(uint16 *frameBuffer, int x, int y, int color565, uint32 codePoint)
{
    int w = 0, h = 0, offX = 0, offY = 0, advance = 0;
    const uint8 *bmp = ui3dsGlyphGet(codePoint, &w, &h, &offX, &offY, &advance);

    if (bmp == NULL)
    {
        ui3dsDrawMissingGlyphBox(frameBuffer, x, y, color565);
        return UI3DS_CJK_CELL;          // 与测宽口径一致（方框边长 = 汉字步进）
    }

    if (w <= 0 || h <= 0)
        return (advance > 0) ? advance : fontHeight;   // 空白字形（空格等）

    int bx = x + offX;
    int by = y + offY;

    for (int gy = 0; gy < h; gy++)
    {
        int cy = by + gy;
        if (cy < viewportY1 || cy >= viewportY2) continue;

        for (int gx = 0; gx < w; gx++)
        {
            uint8 alpha = bmp[gy * w + gx];
            if (alpha == 0) continue;

            int cx = bx + gx;
            if (cx < viewportX1 || cx >= viewportX2) continue;

            if (alpha >= UI3DS_GLYPH_ALPHA_MAX)
                ui3dsSetPixelInline(frameBuffer, cx, cy, color565);
            else
                ui3dsSetPixelInline(frameBuffer, cx, cy,
                    ui3dsApplyAlphaToColour565(color565, alpha) +
                    ui3dsApplyAlphaToColour565(ui3dsGetPixelInline(frameBuffer, cx, cy),
                                               MAX_ALPHA - alpha));
        }
    }

    return (advance > 0) ? advance : w;
}

// 用内置 GBK 点阵绘制。命中返回 true。
// 注意：这里**不**返回笔位步进 —— 前进量统一由 ui3dsMeasureCharCJK 给出，
// 从根本上杜绝"测宽与绘制口径不一致"导致的对齐/换行错位。
static inline bool ui3dsTryDrawBitmap(uint16 *frameBuffer, int x, int y, int color565,
                                      uint32 cp, int len)
{
    if (len != 3)
        return false;

    uint16 chGBK = 0;
    if (!ui3dsGbkHasGlyph(cp, &chGBK))
        return false;

    ui3dsDrawGBK(frameBuffer, x, y, color565, chGBK);
    return true;
}

// 用 3DS 系统共享字体绘制。命中返回 true（前进量同样由 measure 统一给出）。
static inline bool ui3dsTryDrawSystem(uint16 *frameBuffer, int x, int y, int color565,
                                      uint32 cp, int len)
{
    if (!ui3dsGlyphAvailable())
        return false;
    if (!ui3dsGlyphHas(cp))
        return false;

    ui3dsDrawSystemGlyph(frameBuffer, x, y, color565, cp);
    return true;
}


//---------------------------------------------------------------
// Computes width of the string
//---------------------------------------------------------------
int ui3dsGetStringWidth(const char *s, int startPos = 0, int endPos = 0xffff)
{
    int totalWidth = 0;
    uint32 prevCp = 0;              // 行首无前字符
    for (int i = startPos; i <= endPos; )
    {
        uint8 c = (uint8)s[i];
        if (c == 0)
            break;

        int len = 1;
        totalWidth += ui3dsMeasureCharCJK(s, i, &len, prevCp);
        if (len < 1) len = 1;

        int dummy = 1;
        prevCp = (c < 0x80) ? (uint32)c : ui3dsUtf8Decode(s, i, &dummy);

        i += len;
    }
    return totalWidth;
}

#define CONVERT_TO_565(x)    (((x & 0xf8) >> 3) | (((x >> 8) & 0xf8) << 3) | (((x >> 16) & 0xf8) << 8))

//---------------------------------------------------------------
// Colors are in the 888 (RGB) format.
//---------------------------------------------------------------
void ui3dsSetColor(int newForeColor, int newBackColor)
{
    foreColor = newForeColor;
    backColor = newBackColor;
}


//---------------------------------------------------------------
// Draws a rectangle with the color (in RGBA8 format).
//
// Note: x0,y0 are inclusive. x1,y1 are exclusive.
//---------------------------------------------------------------
static void ui3dsDraw32BitRect(uint32 * fb, int x0, int y0, int x1, int y1, int color, float alpha)
{
    uint32 c = ui3dsApplyAlphaToColor(color, alpha) << 8;
    
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int fbofs = x * 240 + (239 - y);
            fb[fbofs] = c;
        }
    }
}

//---------------------------------------------------------------
// Draws a rectangle with the colour (in RGB888 format).
//
// Note: x0,y0 are inclusive. x1,y1 are exclusive.
//---------------------------------------------------------------
void ui3dsDrawRect(int x0, int y0, int x1, int y1, int color, float alpha)
{
    if (color < 0)
        return;

    x0 += translateX;
    x1 += translateX;
    y0 += translateY;
    y1 += translateY;

    if (x0 < viewportX1) x0 = viewportX1;
    if (x1 > viewportX2) x1 = viewportX2;
    if (y0 < viewportY1) y0 = viewportY1;
    if (y1 > viewportY2) y1 = viewportY2;

    if (alpha < 0) alpha = 0;
    if (alpha > 1.0f) alpha = 1.0f;

    uint16* fb = (uint16 *) gfxGetFramebuffer(screenSettings.SecondScreen, GFX_LEFT, NULL, NULL);
    if (gfxGetScreenFormat(screenSettings.SecondScreen) == GSP_RGBA8_OES) {
        return ui3dsDraw32BitRect((uint32 *)fb, x0, y0, x1, y1, color, alpha);
    }
    color = CONVERT_TO_565(color);

    if (alpha == 1.0f)
    {
        for (int x = x0; x < x1; x++)
        {
            int fbofs = (x) * 240 + (239 - y0);
            for (int y = y0; y < y1; y++)
                fb[fbofs--] = color;
        }
    }
    else if (alpha == 0.0)
    {
        return;
    }
    else
    {
        int iAlpha = alpha * MAX_ALPHA;
        for (int x = x0; x < x1; x++)
        {
            int fbofs = (x) * 240 + (239 - y0);
            for (int y = y0; y < y1; y++)
            {
                fb[fbofs] =
                    ui3dsApplyAlphaToColour565(color, iAlpha) +
                    ui3dsApplyAlphaToColour565(fb[fbofs], MAX_ALPHA - iAlpha);
                fbofs --;
            }
        }
    }
}



//---------------------------------------------------------------
// Draws a rectangle with the back colour
//
// Note: x0,y0 are inclusive. x1,y1 are exclusive.
//---------------------------------------------------------------
void ui3dsDrawRect(int x0, int y0, int x1, int y1)
{
    ui3dsDrawRect(x0, y0, x1, y1, backColor, 1.0f);
}


//---------------------------------------------------------------
// Draws a string at the given position without translation.
//---------------------------------------------------------------
void ui3dsDrawStringOnly(uint16 *fb, int absoluteX, int absoluteY, int color, const char *buffer, int startPos = 0, int endPos = 0xffff)
{
    int x = absoluteX;
    int y = absoluteY;

    if (color < 0)
        return;
    if (y >= viewportY1 - 16 && y <= viewportY2)
    {
        color = CONVERT_TO_565(color);
        uint32 prevCp = 0;              // 行首无前字符
        for (int i = startPos; i <= endPos; )
        {
            uint8 c = (uint8)buffer[i];
            if (c == 0)
                break;

            int len = 1;
            uint32 cp = (c < 0x80) ? (uint32)c : ui3dsUtf8Decode(buffer, i, &len);
            if (len < 1) len = 1;

            // ---------------- 绘制 ----------------
            if (c < 0x80 || cp >= UI3DS_LEGACY_BYTE_BASE)
            {
                // ASCII 与旧式单字节字形（UI 图标：复选框 0xFD/0xFE、
                // 上下箭头 0xF8/0xF9、进度条 0xFA..0xFC）都沿用原有单字节点阵，
                // 显示效果与改造前完全一致。
                if (c != 32)
                    ui3dsDrawChar(fb, x, y, color, c);
            }
            else if (!(UI3DS_PREFER_BITMAP_FONT
                       ? (ui3dsTryDrawBitmap(fb, x, y, color, cp, len) ||
                          ui3dsTryDrawSystem(fb, x, y, color, cp, len))
                       : (ui3dsTryDrawSystem(fb, x, y, color, cp, len) ||
                          ui3dsTryDrawBitmap(fb, x, y, color, cp, len))))
            {
                // 两级来源都缺字：系统字体可用时画空心方框（一眼分辨"字库确实没有"）；
                // 连系统字体都不可用，才退回原逐字节行为（不崩，但会显示成西欧字符）
                if (ui3dsGlyphAvailable())
                    ui3dsDrawMissingGlyphBox(fb, x, y, color);
                else if (c != 32)
                    ui3dsDrawChar(fb, x, y, color, c);
            }

            // ---------------- 前进 ----------------
            // 唯一口径是 ui3dsMeasureCharCJK（含中西文自动间距）。
            // 居中、右对齐、换行用的也是它，因此三者不可能对不上。
            int mlen = 1;
            x += ui3dsMeasureCharCJK(buffer, i, &mlen, prevCp);
            if (mlen < 1) mlen = 1;

            prevCp = cp;
            i += mlen;
        }
    }
}


//---------------------------------------------------------------
// Draws a string with the forecolor, with wrapping
//---------------------------------------------------------------
void ui3dsDrawStringWithWrapping(int x0, int y0, int x1, int y1, int color, int horizontalAlignment, const char *buffer_in, bool isTextCFormat)
{
    int strLineCount = 0;
    int strLineStart[30];
    int strLineEnd[30];

    x0 += translateX;
    x1 += translateX;
    y0 += translateY;
    y1 += translateY;

    ui3dsPushViewport(x0, y0, x1, y1);
    //ui3dsDrawRect(x0, y0, x1, y1, backColor);  // Draw the background color

    if (buffer_in != NULL)
    {
        char *buffer = (settings3DS.Language != 0 && !isTextCFormat) ? (char *) getTextFromMap(buffer_in) : (char *) buffer_in;
        int maxWidth = x1 - x0;
        int slen = strlen(buffer);

        int curStartPos = 0;
        int curEndPos = slen;
        uint16 lineWidth = 0;
        uint32 prevCp = 0;              // 行内前一字符的码位（用于中西文自动间距）
        for (int i = 0; i <= slen; )
        {
            if (i != curStartPos)
            {
                if (buffer[i] == ' ' && i > 0 && buffer[i-1] != ' ')
                    curEndPos = i - 1;
                else if (buffer[i] == '-')  // use space or dash as line breaks
                    curEndPos = i;
                else if (buffer[i] == '\n')  // \n as line breaks.
                {
                    curEndPos = i - 1;
                    lineWidth = 65535;     // force the line break.
                }
                else if ((uint8)buffer[i] >= 0x80)
                    curEndPos = i - 1;     // CJK：汉字之间没有空格，必须允许在
            }                              //   任意汉字前断行，否则整句中文会被
                                           //   当成一个长"单词"而完全不换行。

            // 与 ui3dsGetStringWidth 使用同一套宽度口径，否则居中/右对齐会歪
            int clen = 1;
            lineWidth += (uint16)ui3dsMeasureCharCJK(buffer, i, &clen, prevCp);
            if (clen < 1) clen = 1;

            // 记录本字符码位，供下一次迭代判断是否需要中西文间距
            {
                uint8 ic = (uint8)buffer[i];
                int dummy = 1;
                prevCp = (ic < 0x80) ? (uint32)ic : ui3dsUtf8Decode(buffer, i, &dummy);
            }

            if (clen > 1) i += (clen - 1);      // 配合循环末尾的 i++

            if (lineWidth > maxWidth)
            {
                // Break the line here
                strLineStart[strLineCount] = curStartPos;
                strLineEnd[strLineCount] = curEndPos;
                strLineCount++;

                if (strLineCount >= 30) break;

                if (lineWidth != 65535)
                {
                    i = curEndPos + 1;
                    // 对齐到 UTF-8 字符边界，避免下一行从多字节字符中间开始
                    while (i < slen && ((uint8)buffer[i] & 0xC0) == 0x80)
                        i++;
                    while (buffer[i] == ' ')
                        i++;
                } else
                {
                    i = curEndPos + 2;
                    while (i < slen && ((uint8)buffer[i] & 0xC0) == 0x80)
                        i++;
                }
                curStartPos = i;
                curEndPos = slen;
                lineWidth = 0;
                prevCp = 0;             // 新行开始，间距重新累计
            } else
                i++;
        }

        // Output the last line.
        curEndPos = slen;
        if (curStartPos <= curEndPos)
        {
            strLineStart[strLineCount] = curStartPos;
            strLineEnd[strLineCount] = curEndPos;
            strLineCount++;
        }

        uint16* fb = (uint16 *) gfxGetFramebuffer(screenSettings.SecondScreen, GFX_LEFT, NULL, NULL);
        for (int i = 0; i < strLineCount; i++)
        {
            int x = x0;
            if (horizontalAlignment >= 0)
            {
                int sWidth = ui3dsGetStringWidth(buffer, strLineStart[i], strLineEnd[i]);

                if (horizontalAlignment == 0)   // center aligned
                    x = (maxWidth - sWidth) / 2 + x0;
                else                            // right aligned
                    x = maxWidth - sWidth + x0;
            }

            ui3dsDrawStringOnly(fb, x, y0, color, buffer, strLineStart[i], strLineEnd[i]);
            // 对话框内多行文本的行距：与菜单行距解耦（见 UI3DS_WRAPPED_LINE_PITCH 说明），
            // 因为文本框高度固定为 40px，跟着菜单行距放大会把最后一行裁掉。
            y0 += UI3DS_WRAPPED_LINE_PITCH;
        }
    }

    ui3dsPopViewport();
}


//---------------------------------------------------------------
// Draws a string with the forecolor, with no wrapping
//---------------------------------------------------------------
void ui3dsDrawStringWithNoWrapping(int x0, int y0, int x1, int y1, int color, int horizontalAlignment, const char *buffer_in, int startPos, bool isTextCFormat)
{
    x0 += translateX;
    x1 += translateX;
    y0 += translateY;
    y1 += translateY;

    ui3dsPushViewport(x0, y0, x1, y1);
    //ui3dsDrawRect(x0, y0, x1, y1, backColor);  // Draw the background color

    if (buffer_in != NULL)
    {
        char *buffer = (char *) ((settings3DS.Language != 0 && !isTextCFormat) ? &getTextFromMap(buffer_in)[startPos] : &buffer_in[startPos]);
        uint16* fb = (uint16 *) gfxGetFramebuffer(screenSettings.SecondScreen, GFX_LEFT, NULL, NULL);
        int maxWidth = x1 - x0;
        int x = x0;
        if (horizontalAlignment >= HALIGN_CENTER)
        {
            int sWidth = ui3dsGetStringWidth(buffer);

            if (horizontalAlignment == HALIGN_CENTER)   // center aligned
                x = (maxWidth - sWidth) / 2 + x0;
            else                                        // right aligned
                x = maxWidth - sWidth + x0;
        }
        ui3dsDrawStringOnly(fb, x, y0, color, buffer);
    }

    ui3dsPopViewport();
}


//---------------------------------------------------------------
// Copies pixel data from the frame buffer to another buffer
//---------------------------------------------------------------
void ui3dsCopyFromFrameBuffer(uint16 *destBuffer)
{
    uint16* fb = (uint16 *) gfxGetFramebuffer(screenSettings.SecondScreen, GFX_LEFT, NULL, NULL);
    memcpy(destBuffer, fb, screenSettings.SecondScreenWidth * 240 * 2);
}


//---------------------------------------------------------------
// Copies pixel data from the buffer to the framebuffer
//---------------------------------------------------------------
void ui3dsBlitToFrameBuffer(uint16 *srcBuffer, float alpha)
{
    uint16* fb = (uint16 *) gfxGetFramebuffer(screenSettings.SecondScreen, GFX_LEFT, NULL, NULL);

    int a = (int)(alpha * MAX_ALPHA);
    for (int x = viewportX1; x < viewportX2; x++)
        for (int y = viewportY1; y < viewportY2; y++)
        {
            int ofs = ui3dsComputeFrameBufferOffset(x,y);
            int color = ui3dsApplyAlphaToColour565(srcBuffer[ofs], a);
            fb[ofs] = color;
        }
}
