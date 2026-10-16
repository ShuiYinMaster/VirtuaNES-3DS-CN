#include <cstdio>
#include <cstdlib>

#include <stdio.h>
#include <string.h>
#include <3ds.h>

#include "3dstypes.h"

uint8 *fontGBK = NULL;
uint8 *fontGBKUsed = NULL;

//---------------------------------------------------------------------------
// 建立"槽位是否有字形"位图。
//
// 字库里 8178 个槽位中有 688 个是 GB2312 未定义位置（整槽全 0）。必须把它们
// 区分出来，否则查表会认为"字库有这个字"，绘制结果是空白 —— 既看不出缺字，
// 也拿不到系统字体的兜底。位图只占 1023 字节，查询 O(1)。
//---------------------------------------------------------------------------
static void buildUsedMap(const uint8 *data, int slots) {
	const int bytes = (slots + 7) / 8;

	if (fontGBKUsed != NULL) {
		free(fontGBKUsed);
		fontGBKUsed = NULL;
	}

	fontGBKUsed = (uint8 *)calloc(bytes, 1);
	if (fontGBKUsed == NULL)
		return;

	for (int i = 0; i < slots; i++) {
		const uint8 *g = data + i * 256;
		for (int k = 0; k < 256; k++) {
			if (g[k] != 0) {
				fontGBKUsed[i >> 3] |= (uint8)(1u << (i & 7));
				break;
			}
		}
	}
}

bool gbk3dsSlotUsed(int slot) {
	if (slot < 0)
		return false;
	if (fontGBKUsed == NULL)
		return true;            // 位图未建立：不做判断，保持旧行为
	return (fontGBKUsed[slot >> 3] >> (slot & 7)) & 1;
}

//---------------------------------------------------------------------------
// 1bit 打包格式支持
//
// 进 romfs 的字库是按 1bit 打包的（见 _scratch/pack_gbk.py）：
//     offset 0 : 4 字节 magic "GBK1"
//     offset 4 : uint32 LE = 解包后大小
//     offset 8 : 位流，LSB 优先；第 i 位对应解包后第 i 字节
//
// 之所以能这样压：该字库 alpha 只有 0 和 8 两个取值（纯双色、零抗锯齿），
// 1 个 bit 无损表达。2,093,568 -> 261,704 字节（8.00x），
// 让 3dsx 从 3.39MB 降到 1.56MB 级别 —— 这对 NetLoader 无线推送是决定性的。
//
// 打包与未打包两种格式都支持：先按原始大小判，再按 magic 判。
// 用户在 SD 卡上放未打包的原版 gbk.bin 也照常能用。
//---------------------------------------------------------------------------
#define GBK_PACK_MAGIC      "GBK1"
#define GBK_PACK_HEADER     8

static bool gbkUnpackBits(const uint8 *src, size_t srcBytes, uint8 *dst, int dstBytes) {
	if ((size_t)((dstBytes + 7) / 8) > srcBytes)
		return false;

	for (int i = 0; i < dstBytes; i++) {
		uint8 b = src[i >> 3];
		dst[i] = ((b >> (i & 7)) & 1) ? 8 : 0;
	}
	return true;
}

//---------------------------------------------------------------------------
// 字库搜索顺序
//
//   1) romfs:/gbk.bin        —— 随 3dsx 一起分发，用户无需在 SD 卡上放文件。
//                               构建时由 --romfs 打进二进制（见 assets/romfs/）。
//   2) sdmc:/emus3ds/gbk.bin —— SD 卡版本，便于用户自行替换成更大的字库。
//   3) /emus3ds/gbk.bin      —— 兼容旧路径写法（libctru 默认设备即 sdmc）。
//
// 三处都没找到时不报错：CJK 会改由 3DS 系统共享字体渲染（覆盖率更高，
// 只是边缘略柔），保证任何情况下都不会退化成乱码。
//---------------------------------------------------------------------------
static const char *gbkSearchPaths[] = {
	"romfs:/gbk.bin",
	"sdmc:/emus3ds/gbk.bin",
	"/emus3ds/gbk.bin",
};

void gbk3dsLoadGBKImage() {
	const int fontGridSize = 16;
	const int gbkRowStart = 0xa1;
	const int gbkRowEnd = 0xf7;
	const int gbkColStart = 0xa1;
	const int gbkColEnd = 0xfe;

	const int rowCount = (gbkRowEnd - gbkRowStart + 1);
	const int colCount = (gbkColEnd - gbkColStart + 1);
	const int rstBufSize = rowCount * colCount * (fontGridSize * fontGridSize);

	// 允许被重复调用（切换字库/重载）：先释放旧数据
	if (fontGBK != NULL) {
		free(fontGBK);
		fontGBK = NULL;
	}
	if (fontGBKUsed != NULL) {
		free(fontGBKUsed);
		fontGBKUsed = NULL;
	}

	const unsigned pathCount = sizeof(gbkSearchPaths) / sizeof(gbkSearchPaths[0]);
	for (unsigned i = 0; i < pathCount; i++) {
		FILE *file = fopen(gbkSearchPaths[i], "rb");
		if (file == NULL)
			continue;

		fseek(file, 0, SEEK_END);
		off_t size = ftell(file);
		fseek(file, 0, SEEK_SET);

		if (size <= 0) {
			fclose(file);
			continue;
		}

		uint8 *raw = (uint8 *)malloc((size_t)size);
		if (raw == NULL) {
			fclose(file);
			continue;
		}

		off_t bytesRead = fread(raw, sizeof(uint8), (size_t)size, file);
		fclose(file);

		if (bytesRead != size) {
			printf("gbk: read failed in %s\n", gbkSearchPaths[i]);
			free(raw);
			continue;
		}

		uint8 *buf = NULL;

		if ((size_t)size == (size_t)rstBufSize) {
			// 原样点阵：直接接管这块内存
			buf = raw;
			raw = NULL;
			printf("gbk: %s (raw)\n", gbkSearchPaths[i]);
		} else if (size >= GBK_PACK_HEADER &&
		           memcmp(raw, GBK_PACK_MAGIC, 4) == 0) {
			// 1bit 打包：校验头部声明的大小，再解包
			uint32 declared = (uint32)raw[4]
			                | ((uint32)raw[5] << 8)
			                | ((uint32)raw[6] << 16)
			                | ((uint32)raw[7] << 24);

			if (declared == (uint32)rstBufSize) {
				buf = (uint8 *)malloc(rstBufSize);
				if (buf != NULL) {
					if (gbkUnpackBits(raw + GBK_PACK_HEADER,
					                  (size_t)size - GBK_PACK_HEADER,
					                  buf, rstBufSize)) {
						printf("gbk: %s (1bit packed, %d -> %d)\n",
						       gbkSearchPaths[i], (int)size, rstBufSize);
					} else {
						printf("gbk: %s packed data truncated\n", gbkSearchPaths[i]);
						free(buf);
						buf = NULL;
					}
				}
			}
		}

		if (raw != NULL)
			free(raw);

		if (buf == NULL) {
			printf("gbk: %s size mismatch (%d, expected %d or packed)\n",
			       gbkSearchPaths[i], (int)size, rstBufSize);
			continue;
		}

		fontGBK = buf;
		buildUsedMap(fontGBK, rowCount * colCount);
		return;
	}

	printf("gbk: no bitmap font found, CJK falls back to system shared font\n");
}
