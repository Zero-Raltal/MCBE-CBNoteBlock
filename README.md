# Minecraft BE 1.19+ 音乐命令方块生成器

> 基于 MIDI / NBS 文件，一键生成基岩版命令方块音乐序列

[![Version](https://img.shields.io/badge/version-1.0.0-blue)](https://github.com/ZeroRaltal/MusicCommandGenerator)
[![License](https://img.shields.io/badge/license-MIT-green)](./LICENSE)

---

## 简介

这是一个**纯前端**的 Web 工具，支持将 **MIDI (.mid/.midi)** 和 **Note Block Studio (.nbs)** 格式的音乐文件，自动转换为 Minecraft **基岩版 1.19+** 的命令方块指令序列。  
生成的指令可直接复制到游戏内，配合 **循环命令方块** 和 **连锁命令方块** 播放音符，实现红石音乐自动化。

---

## 功能特点

- **双格式支持** – 解析 `.mid` / `.midi` / `.nbs` 文件（兼容 NBS V0/V1/V2）。
- **乐器映射** – 将 GM/打击乐映射至 Minecraft 原版音色（如 harp、bell、flute、bd、snare 等）。
- **生成命令** – 自动生成初始化、循环、连锁命令方块指令，并按时间点去重。
- **实时预览** – 使用 Web Audio API 在浏览器中播放音乐，支持速度调节、进度拖拽。
- **八度移调** – 整体升降八度，适应不同音域。
- **两种模式** – “乐器映射”保留原声，“Harp Only”强制使用竖琴音色（适用于纯音符方块）。
- **便捷复制** – 每条指令独立复制，或一键复制全部链式指令。
- **导出文件** – 将生成的命令保存为 `.txt` 文件，方便游戏内导入。

---

## 如何使用

### 1. 在线使用（推荐）
<a href="https://zero-raltal.github.io/MCBE-CBNoteBlock/" style="display:inline-block;background:#3e8e62;color:#fff;padding:10px 20px;text-decoration:none;border-radius:4px;font-weight:bold;">点击前往</a>

### 2. 本地运行
将 `index.html` 下载到本地，用现代浏览器（Chrome / Edge / Firefox / Safari）打开即可，无需任何后端环境。

### 3. 操作流程
1. **上传文件**：点击“上传”按钮，选择 `.mid` / `.midi` / `.nbs` 文件。
2. **调整参数**（可选）：
   - 切换**音色模式**（乐器映射 / Harp Only）。
   - 拖动**八度移调**滑块调整整体音高。
   - 设置**预览速度**以加速或减速试听。
3. **预览音乐**：点击“播放”按钮试听效果，可随时停止或拖动进度条跳转。
4. **生成指令**：页面下方自动生成 **循环命令方块** 和 **连锁命令方块** 的指令列表。
5. **复制使用**：
   - 复制循环指令，填入**循环命令方块**。
   - 复制所有连锁指令（或逐条复制），依次放入**连锁命令方块**（注意顺序）。
6. **游戏内设置**（首次使用）：
   - 创建记分板：`/scoreboard objectives add t dummy`
   - 初始化分数：`/scoreboard players set @a t 0`
   - 循环方块：`scoreboard players add @a t 1`
   - 连锁方块：粘贴生成的 `execute as ...` 指令。

---

## 技术实现

- **MIDI 解析**：原生 JavaScript 读取 MIDI 文件结构，提取音符、时长、通道、音色变化（Program Change）及 Tempo 事件。
- **NBS 解析**：兼容 Note Block Studio 格式（V0~V6），支持自定义乐器、音高、音量、声像。
- **音频合成**：基于 Web Audio API 的 `OscillatorNode` 和 `PeriodicWave`，模拟不同音色的频谱特性（含基频与泛音列）。
- **命令生成**：将音符按 `(instrument, pitch)` 分组，剔除重复时间点，生成 `execute unless entity @s[scores={...}]` 条件执行指令，确保同一 tick 只触发一次。
- **UI 交互**：响应式设计，适配电脑与移动设备；提供进度条拖拽、速度滑块等交互反馈。

---

## 注意事项

- **浏览器要求**：需支持 ES6、Web Audio API 和 `DataView`、`Uint8Array` 等 TypedArray。
- **游戏版本**：指令基于 **基岩版 1.19+** 的 `playsound` 语法，其他版本可能不兼容。
- **性能建议**：音符数量较多（>5000）时，预览和生成可能稍有延迟，建议使用较新的设备。
- **版权声明**：作者保留署名权，您可以在遵守 MIT 协议的前提下自由使用、修改和分发。

---

## 贡献

欢迎提交 Issue 或 Pull Request，帮助改进项目：

- 报告 Bug（请附上导致问题的文件及操作步骤）。
- 建议新功能（如更多音色映射、其他曲谱格式支持等）。
- 完善文档或翻译。

---

## 许可证

本项目采用 **MIT 许可证**，详情见 [LICENSE](./LICENSE) 文件。
