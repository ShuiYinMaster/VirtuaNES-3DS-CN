# VirtuaNES for 3DS · 中文增强版

基于 [R-YaTian/emus3ds](https://github.com/R-YaTian/emus3ds) 的改进分支（基线 `a10138f`），
面向 VirtuaNES（NES / FC）核心的中文显示与中西文混排排版。

> 原始项目由 [bubble2k16](https://github.com/bubble2k16) 开发；
> 中文目录支持与 GBK 字库来自 [xxxxst](https://github.com/xxxxst)；
> 中文界面、拼音排序与 i18n 由 [R-YaTian](https://github.com/R-YaTian) 完成。

## 特性

### 字库内嵌进 romfs

字库随 3dsx / CIA 一起分发，**不需要在 SD 卡上单独放置字体文件**。

- 1 bit 打包：2,093,568 → **261,704 字节**（magic `GBK1`：u32 解包大小 + LSB 位流）
- 加载顺序 `romfs:` → SD 卡 → 旧路径，兼容把字体放在 SD 卡的环境
- 打包前的字库为 16×16 点阵（87 行 × 94 列，覆盖 GB2312 全部区段）

### 字形两级来源

中文字形按 Unicode 码位渲染，来源两级：

| 优先级 | 来源 | 特点 |
|---|---|---|
| 首选 | 内嵌 12px GBK 原生点阵 | 双色点阵、零抗锯齿、与英文点阵同尺寸，边缘锐利 |
| 兜底 | 3DS 系统共享字体（`fontEnsureMapped` + A4 4bpp 面积平均下采样） | 覆盖 GB2312 之外的字（生僻字、繁体、假名等） |

- GB2312 区有 688 个未定义空槽，用 `fontGBKUsed` 位图表标记，空槽回落系统字体，
  避免画出空白
- 汉字覆盖率 **6763 / 6768 = 99.9%**

### 中西文混排排版

- 菜单行距 13px；列表可见行数由内容区高度反推，绘制端再兜底夹取
  （`while (startY + n*行距 > 内容区底) n--`），保证最后一行不越过内容区边界
- 中西文交界插入 1px 自动间距（对应传统排版的"自动调整中文与西文间距"）
- 对话框换行文本的行距与菜单行距解耦（13px），保证三行文本不被裁切

### UI 图标字形

设置项里的复选框、翻页箭头、进度条是**单字节字形索引**，写在内置字体的第 248–254 号槽位：

| 字节 | 用途 |
|---|---|
| `0xF8` / `0xF9` | 上 / 下翻页箭头 |
| `0xFA` / `0xFB` / `0xFC` | 进度条指针 / 条身 |
| `0xFD` / `0xFE` | 复选框「已勾选」/「未勾选」 |

渲染时用哨兵 `UI3DS_LEGACY_BYTE_BASE` 识别"不构成合法 UTF-8 的单字节"，
回落到内置字体的单字节点阵，与汉字、西文的按码位渲染共存。

### 默认字体与配置迁移

- 默认字体 `Font = 0`（Tempesta）
- 配置读取列表**末尾**新增 `FontRev` 迁移键，用于把历史配置里的字体设置纠正为默认值：
  老配置文件没有该键 → 强制 `Font = 0`；保存后写入 `FontRev=1`

  > 该键必须位于列表**最末尾**。配置格式用 `fscanf` 顺序读取，键名与顺序必须和写入严格对齐，
  > 插在中间会导致老配置文件的后续设置全部读取失败。

## 构建

需要 [devkitPro](https://devkitpro.org/)（devkitARM + libctru + citro3d + picasso + 3dstools）：

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH

# VirtuaNES 核心（本分支的主要目标）
make -f virtuanes-make -j8
```

产物：
- `virtuanes_3ds.3dsx` —— Homebrew Launcher 用
- `virtuanes_3ds.cia` —— 装到主菜单用（由 Makerom 自动打包，romfs 字库已内嵌）

TemperPCE / PicoDrive 核心分别用 `temperpce-make` / `picodrive-make`。

> 构建提示：`src/cores/virtuanes/assets/romfs/gbk.bin` 必须是**已打包格式**
> （`GBK1` magic + u32 解包大小 + LSB 位流）。仓库根目录的 `gbk.bin` 是
> 16×16 原始点阵（87×94×256）字库源。

## 安装

**方式一：CIA（装到主菜单）**
用 **FBI** 安装 `virtuanes_3ds.cia`。

**方式二：3dsx（无需安装）**
`virtuanes_3ds.3dsx` 放到 SD 卡 `/3ds/`，从 Homebrew Launcher 启动；
或用 hbmenu 的 **NetLoader** 无线推送（3DS 上按 `Y`，
电脑上 `3dslink -a <IP> virtuanes_3ds.3dsx`）。

**方式三：FBI 无线远程安装（不用拔卡）**
3DS 上 `FBI` → `Remote Install` → `Receive URLs over the network`，
再把 cia 的 URL 投递给 3DS 的 5000 端口，3DS 上按 `A` 确认。

以上三种方式都**不需要额外往 SD 卡放字体文件**。

## 发布产物

见 [Releases](../../releases)。当前版本：

| 文件 | 大小 | sha256 |
|---|---|---|
| `virtuanes_3ds.3dsx` | 1,558,176 | `a164c1af9c808c355cfcf208465c6c4dbc4df6572c5c9e80fec6277b3fb26212` |
| `virtuanes_3ds.cia` | 1,577,920 | `23cc4607f310d773d801e40d0c969c5eb575f289ca8abce571f1cc26bc296ebf` |

CIA 信息：Title ID `0x0004000000384A00`，Product Code `CTR-P-VNES`，
未加密（需 CFW 已开启签名校验补丁，Luma3DS + FBI 默认即可）。

## 致谢

- [bubble2k16](https://github.com/bubble2k16) —— 原版作者
- [xxxxst](https://github.com/xxxxst) —— 中文目录支持与 GBK 字库
- [R-YaTian](https://github.com/R-YaTian) —— 中文界面、拼音排序、i18n
- 翻译：James-Makoto、R-YaTian、xxxxst

各核心的上游项目：VirtuaNES（Norix）、Temper（Exophase）、PicoDrive（notaz）。

## 许可证

本项目遵循上游仓库的授权状况，**未新增任何限制**。
上游未附独立的 LICENSE 文件，请同时参阅
[bubble2k16/emus3ds](https://github.com/bubble2k16/emus3ds) 与
[R-YaTian/emus3ds](https://github.com/R-YaTian/emus3ds) 的说明。
各模拟器核心的著作权归其原作者所有。
