# emus3ds 中文增强版 · VirtuaNES for 3DS

本仓库是 [R-YaTian/emus3ds](https://github.com/R-YaTian/emus3ds) 的**中文本地化改进分支**，
重点解决 3DS 上**中文显示为乱码**的问题，并修正了若干 UI 排版缺陷。

> 原始项目由 [bubble2k16](https://github.com/bubble2k16) 开发；
> 中文目录支持与 GBK 字库思路来自 [xxxxst](https://github.com/xxxxst)；
> 中文化持续推进、拼音排序与 i18n 由 [R-YaTian](https://github.com/R-YaTian) 完成。
> 本分支在其基础上继续改进，**保留全部原始署名与致谢**。

## 本分支做了什么

### 1. 中文渲染：内置 12px GBK 原生点阵 + 系统共享字体兜底

原实现 `ui3dsDrawChar(..., uint8 c)` 用**单字节**索引 256 项字形表，
中文 UTF-8 的三字节序列会落进 128–255 的西欧重音字符槽位 —— 这就是乱码的根因。

本分支把渲染改为**按 Unicode 码位**，字形来源两级：

| 优先级 | 来源 | 特点 |
|---|---|---|
| 首选 | 内置 12px GBK 原生点阵（`src/cores/virtuanes/assets/romfs/gbk.bin`） | 双色点阵、零抗锯齿、与英文点阵同尺寸，**边缘锐利** |
| 兜底 | 3DS 系统共享字体（`fontEnsureMapped` + A4 4bpp 面积平均下采样） | 覆盖 GB2312 之外的一切（生僻字、繁体、假名等） |

- 字库经 1bit 打包：2,093,568 → **261,704 字节**（magic `"GBK1"`）
- GB2312 区有 688 个未定义空槽，用位图表 `fontGBKUsed` 标记，
  空槽自动回落系统字体，避免"看似有字实为空白"
- 汉字覆盖率：**6763 / 6768 = 99.9%**

### 2. 中西文混排排版

- 菜单行距 13px；列表可见行数**由内容区高度反推**，
  并在绘制端兜底夹取 `while (startY + n*行距 > 内容区底) n--`，
  从根上杜绝"列表侵入页脚状态栏"
- 中西文交界插入 1px 自动间距（对应传统排版的"自动调整中文与西文间距"）
- 对话框换行文本行距与菜单行距解耦（13px），保证三行不被裁切

### 3. 修正的缺陷

| 症状 | 真因 | 修法 |
|---|---|---|
| 设置项出现**空心矩形**（复选框/箭头/进度条） | UI 图标是**单字节字形索引**写进字符串字面量（`0xF8` 上箭头 / `0xF9` 下箭头 / `0xFA..0xFC` 进度条 / `0xFD` 勾选 / `0xFE` 未勾选）。这些字节不是合法 UTF-8，被解码成 `U+FFFD` 后落入"缺字方框"分支 | 设哨兵 `UI3DS_LEGACY_BYTE_BASE`，非法 UTF-8 单字节回落到内置字体单字节点阵 |
| 英文**发虚模糊** | 默认字体曾被改为 Arial —— 半透明像素占比 76.9%，软度是 Tempesta 的 9 倍 | 默认改回 `Font = 0`（Tempesta），并用 `FontRev` 迁移键纠正老配置 |
| 老配置里字体改不回来 | `Font` 是**持久化**设置，只改默认值无效 | 在配置列表**末尾**新增 `FontRev`：老文件无此键 → 强制 `Font=0`；保存后写入 `FontRev=1` |

> ⚠️ `FontRev` 必须加在配置读取列表的**最末尾**：该格式用 `fscanf` 顺序读取，
> 键名/顺序必须与写入严格对齐，插在中间会导致老文件后续设置全部读取失败。

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
- `virtuanes_3ds.3dsx` —— 家用机启动器（Homebrew Launcher）用
- `virtuanes_3ds.cia` —— 已安装到主菜单用（由 `Makerom` 自动打包，romfs 字库已内嵌）

TemperPCE / PicoDrive 核心用 `temperpce-make` / `picodrive-make`。

> 构建环境提示：`src/cores/virtuanes/assets/romfs/gbk.bin` 必须是已打包格式
> （`GBK1` magic + u32 解包大小 + LSB 位流）。仓库根目录的 `gbk.bin`
> 是 16×16 原始点阵（87×94×256）字库源，配套打包脚本见 `_scratch/pack_gbk.py`。

## 安装

**方式一：3dsx（无需安装）**
把 `virtuanes_3ds.3dsx` 放到 SD 卡 `/3ds/` 下，从 Homebrew Launcher 启动；
或用 hbmenu 的 **NetLoader** 无线推送（3DS 上按 `Y`，电脑上 `3dslink -a <IP> virtuanes_3ds.3dsx`）。

**方式二：CIA（装到主菜单）**
把 `virtuanes_3ds.cia` 拷到 SD 卡，用 **FBI** 安装。

**方式三：FBI 无线远程安装（不用拔卡）**
3DS 上 `FBI` → `Remote Install` → `Receive URLs over the network`，
电脑上起一个 HTTP 服务并把 URL 投递给 3DS 的 5000 端口（协议见下），
3DS 上按 `A` 确认即可。

```
PC → TCP <3DS_IP>:5000
     send: struct.pack('!L', len(payload)) + payload
     payload: ASCII，每行一个 URL，形如 <PC_IP>:<HTTP_PORT>/<filename>
             （注意**不带** http:// 前缀）
FBI 逐个 HTTP 下载并安装，完成后回一个确认字节
```

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
- [R-YaTian](https://github.com/R-YaTian) —— 中文化持续推进、拼音排序、i18n
- 翻译：James-Makoto、R-YaTian、xxxxst

各核心的上游项目：VirtuaNES（Norix）、Temper（Exophase）、PicoDrive（notaz）。

## 许可证

本项目遵循上游仓库的授权状况，**未新增任何限制**。
上游未附独立的 LICENSE 文件，请同时参阅
[bubble2k16/emus3ds](https://github.com/bubble2k16/emus3ds) 与
[R-YaTian/emus3ds](https://github.com/R-YaTian/emus3ds) 的说明。
各模拟器核心的著作权归其原作者所有。
