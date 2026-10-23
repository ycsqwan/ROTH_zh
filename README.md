# Realms of the Haunting — 中文化 Mod（实验性）

把 1996 年的 DOS 游戏 **Realms of the Haunting** 中文化。

以 [ROTH.C](https://github.com/slidick/ROTH.C)（引擎 C 语言重制 + mod 平台）为载体，通过一个 32 位
`plugin.dll`（replacement mod）接管引擎的 `draw_text_to_buffer`，用
[stb_truetype](https://github.com/nothings/stb) 把中文点阵字形直接渲染进游戏原生的 **8-bit 调色板帧缓冲**。

**不修改任何原始游戏文件** —— 译文以外部表 `translations_zh.bin` 外挂，字体为独立 TTF 文件，卸载即还原。

> ⚠️ **实验性构建（experimental）**
> 文本翻译覆盖率约 **99.5%**，但**过场 / 开场字幕在游戏内暂不显示**，NPC 对话也**未经完整实测**。
> 欢迎试用与反馈，但请勿视作成品。详见 [已知问题](#已知问题)。

---

## 现状

| 项目 | 状态 |
| --- | --- |
| 覆盖的文本 | DBASE100/400 全部对白与过场字幕 |
| 唯一文本行 | 3297 行，已译 **3281 行（99.5%）** |
| 未译 16 行 | 均为非对白（图形模式标签 `Mode-X/VGA/VESA`、人名 `R. J.`、空格） |
| 字体 | 文泉驿点阵宋体 14px（WenQuanYi Bitmap Song） |
| 渲染方式 | stb_truetype → 8-bit 调色板索引 |
| 覆盖的 UI | 菜单、选项、道具名、对话框（已实测） |
| 未验证 | 过场字幕、NPC 对话（**尚无成功截图**） |

---

## 安装

需要一份 **ROTH.C 引擎**（`rothc.exe`）加 GOG 版游戏数据。

1. 把以下文件放进 `<游戏目录>/ROTH/mods/chinese/`：

   ```
   plugin.dll
   translations_zh.bin
   WenQuanYi Bitmap Song 14px.ttf
   ```

   （目录不存在就新建。引擎按 `mods/<名字>/` 的**字典序**加载，无需清单文件。）

2. 启动 `rothc.exe`。同目录会生成调试日志 `chinese.log`，首行应出现：

   ```
   load: id=io.github.ycsqwan.roth-chinese abi=0.5 ... dir='.../ROTH/mods/chinese'
   font: loaded .../WenQuanYi Bitmap Song 14px.ttf (... scale=0.010 ...)
   tbl: loaded 3281 entries
   ```

3. 在游戏内的**字幕设置**里把字幕全部打开。

> 仓库不含任何原版游戏文件（`DBASE*.DAT`、`*.GDV`、`rothc.exe` 等），需自备。

---

## 构建

需要一个 32 位 mingw-w64 gcc（本工程用 [w64devkit](https://github.com/skeeto/w64devkit)）。

```sh
# 指向你的 ROTH.C 检出目录的 sdk/
make ROTH_SDK=/path/to/ROTH.C/sdk

# 或直接拷进游戏 mods 目录
make install GAME=1
```

产物为 `plugin.dll`（约 155 KB，静态链接，无外部依赖）。

关键编译选项（见 `Makefile`）：

- `-fno-stack-protector` —— override 可从 70 Hz 的 `SIGALRM` tick 到达
  （见 SDK `MODDING_GUIDE.md` 的 *tick_isr_reachable*），信号上下文里不能有栈保护。
- `-fno-asynchronous-unwind-tables` —— 从不抛异常/展开，去掉 `.eh_frame` 以减小体积。
- 未用 `-ffreestanding` —— 本 mod 有意使用 libc（`fopen`/`fprintf` 写日志、`memcpy`、`snprintf`）。

---

## 工作原理

- **接管点**：`draw_text_to_buffer`（canonical VA `0x14d04`）。它是引擎最内层的控制码字符串
  渲染器，菜单、定时消息、对话面板、选项菜单、道具标签都经过它。
- **策略**：*replace*（而非 *wrap*），注册为**最外层**（`priority = 100`，数值越大越先派发）。
  未命中的行通过 `roth_next_draw_text_to_buffer()` 交还给链条，因此优先级更低的 mod 仍能看到
  每一条不属于我们的字符串。
- **字形**：命中译文时，用 stb_truetype 从 CJK 点阵字体栅格化，跳过引擎的单字节字形循环。
- **字体尺寸**：文泉驿点阵宋体是像素网格字体（`unitsPerEm = 1500`，hhea `ascent - descent = 1500`），
  原生尺寸恰为 **15 px**，必须在 `em = 15` 下栅格化，否则覆盖度不再是二值（0/255），
  描边阈值会吃掉整条笔画。
- **游戏内存访问**：`rb()` 读一个字符串字节。落在 game_ram 窗口内的指针走 SDK 的
  边界检查访问器（`api->game_ram->u8`）；窗口外的（exe 自身的字符串/数据页）平坦解引用，
  与引擎自身的 `RB()` 一致。

---

## 已知问题

1. **过场 / 开场字幕不显示**（最主要）。字幕走定时消息路径
   （`layout_timed_message_text` / `run_timed_message_sequence`），
   与对话框不是同一条链，现有 override 可能未覆盖。日志显示
   `draw: 1/1 lines translated`，但画面无字幕 —— 根因待查。
2. **NPC 对话未经完整实测**（仅设置菜单确认显示中文）。
3. **行距重叠**：WQY 14px 墨迹高约 14px，而引擎固定行距 11px，多行中文可能轻微重叠。
4. **闪烁**：部分画面有轻微闪烁。

---

## 字体与许可

- 本 mod 的源码（`plugin.c`、`Makefile`）以 **MIT** 发布，见 [LICENSE](LICENSE)。
- 运行时字体 **文泉驿点阵宋体（WenQuanYi Bitmap Song）** 使用
  **GPL-2.0-with-font-exception** 许可，版权归文泉驿项目所有
  （见 <https://www.wenquanyi.org/>）。本仓库只随附运行时需要的
  `fonts/WenQuanYi Bitmap Song 14px.ttf`。
- 开发期试过的其他字体（Cubic 11、Noto Sans SC、其他像素尺寸）未包含在本仓库内；
  其许可参考见 `fonts/OFL.txt`。
- **不包含任何原版游戏素材。**

---

## 致谢

- [slidick/ROTH.C](https://github.com/slidick/ROTH.C) —— 引擎重制与 mod 平台
- [nothings/stb](https://github.com/nothings/stb) —— stb_truetype
- 文泉驿项目 —— 点阵宋体
- 原版 *Realms of the Haunting* © 1996 Gremlin Interactive / Interplay
