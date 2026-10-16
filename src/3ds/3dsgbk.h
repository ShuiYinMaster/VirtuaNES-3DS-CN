#ifndef _3DSGBK_H_
#define _3DSGBK_H_

#include <3ds.h>

// 字库数据（16x16 x 8bpp，按 (row-0xA1)*94 + (col-0xA1) 索引，每槽 256 字节）
extern uint8 *fontGBK;

// "槽位是否有实际字形" 的位图（1 bit / 槽）。
// GB2312 区里有 688 个未定义位置，这些槽位全为 0 —— 若不区分，
// 它们会被当成"字库里有这个字"，结果画出空白（比画缺字框更难排查）。
extern uint8 *fontGBKUsed;

// 查询某槽位是否真的有字形。位图未建立时一律返回 true（保持旧行为）。
bool gbk3dsSlotUsed(int slot);

void gbk3dsLoadGBKImage();

#endif
