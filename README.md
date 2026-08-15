# <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 3v10.55c-.59-.34-1.27-.55-2-.55-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4V7h4V3h-6z"></svg> Minecraft BE 1.19+ 音乐命令方块生成器

[![在线使用](https://img.shields.io/badge/在线使用-点击跳转-2b8cbe?style=flat-square)](https://zero-raltal.github.io/MCBE-CBNoteBlock/)
[![许可证](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

> 将 MIDI / NBS 文件一键转换为基岩版 1.19+ 的音乐命令方块指令，内置预览播放与瀑布图可视化。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/></svg> 功能特点

- **多格式支持** – 支持上传 `.mid` / `.midi` / `.nbs` 文件，自动解析音符和乐器信息。
- **精确指令生成** – 生成的 `playsound` 命令精确匹配原曲音高与节奏，支持 **乐器映射** 和 **Harp Only** 两种模式。
- **实时预览** – 内置 Web Audio 合成器，可在浏览器中试听生成效果，播放速度可调，支持拖拽进度条跳转。
- **瀑布图可视化** – 动态显示音符序列，当前播放位置高亮，方便查看配器与旋律走向。
- **一键复制/导出** – 每条连锁指令均可独立复制，也可一键导出全部指令为文本文件。
- **主题配色** – 内置经典绿、神秘黄、晴空蓝三套主题，并支持跟随系统深色模式。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/></svg> 在线使用

直接访问：**[https://zero-raltal.github.io/MCBE-CBNoteBlock/](https://zero-raltal.github.io/MCBE-CBNoteBlock/)**

无需安装任何软件，浏览器打开即可使用。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M18 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zM6 4h5v8l-2.5-1.5L6 12V4z"/></svg> 使用方法

### 1. 上传音乐文件
点击「上传文件」按钮，选择你的 `.mid` / `.midi` / `.nbs` 文件。解析成功后，页面会显示音符总数、时长、乐器组等信息。

### 2. 调整参数（可选）
- **音色模式**：  
  - *乐器映射* – 根据 MIDI 程序号或 NBS 乐器 ID 映射到对应的游戏音色（如吉他、长笛、贝斯等）。  
  - *Harp Only* – 将所有非打击乐器统一为竖琴音色，适合简单复刻或节省命令数量。
- **八度移调**：可将所有音符整体移调（±4 个八度），以适应不同音域或避免命令方块音高限制。
- **预览速度**：拖动滑块可调节播放速度（0.1x ~ 5.0x），方便快速审阅。

### 3. 预览音频
点击「播放」按钮即可在浏览器中试听当前生成的音乐。进度条支持点击和拖拽跳转，点击「停止」可停止播放并重置进度。

### 4. 获取命令方块指令
在页面下方 **「1. 聊天栏输入」** 和 **「2. 脉冲命令方块」** 部分，已提供初始设置所需的计分板指令和重置指令。  
在 **「3. 循环命令方块」** 中显示循环指令（每 tick 增加计分板分数）。  
在 **「4. 连锁命令方块」** 中列出所有生成的 `playsound` 连锁命令，每条命令均带有复制按钮。

### 5. 复制或导出
- 点击单条命令旁的「复制」按钮，可复制该条命令。
- 点击「复制循环指令」或「复制全部连锁指令」可批量复制。
- 点击「导出为文件」可将全部命令保存为 `.txt` 文本文件，方便在游戏外编辑。

### 6. 在游戏中布置命令方块
按照页面提示依次放置：
1. **聊天栏**输入计分板创建和初始化命令。  
2. **脉冲命令方块**（紧贴循环命令方块）执行计分板重置命令。  
3. **循环命令方块**（无条件、红石控制）执行累加 tick 的命令。  
4. **连锁命令方块**（无条件、始终活动）按顺序放置，内容即为生成的 `playsound` 命令。  

> <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 2L2 22h20L12 2z M12 15v-4 M12 19h.01"/></svg> 注意：连锁命令方块的顺序不影响播放（因为播放条件基于计分板 `t` 的值）。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8zm0-14c-3.31 0-6 2.69-6 6s2.69 6 6 6 6-2.69 6-6-2.69-6-6-6zm0 10c-2.21 0-4-1.79-4-4s1.79-4 4-4 4 1.79 4 4-1.79 4-4 4z"/></svg> 技术说明

- **MIDI 解析**：支持标准 MIDI 文件（格式 0/1），自动合并多音轨，处理音符开/关事件和程序切换。
- **NBS 解析**：兼容 Note Block Studio 的 v0/v1/v2/v3/v4/v5/v6 版本，读取乐器、层、自定义音色等信息。
- **音频合成**：使用 Web Audio API，采样为内置 OGG 样本（Base91 编码），根据音符频率实时调整播放速率，提取自游戏内音符盒音色。
- **命令生成逻辑**：利用计分板 `t` 记录 tick 计数，每条命令通过 `unless entity` 检测当前 tick 值来精确触发对应音符，避免条件判断冲突。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M9.4 16.6L4.8 12l4.6-4.6L8 6l-6 6 6 6 1.4-1.4zm5.2 0l4.6-4.6-4.6-4.6L16 6l6 6-6 6-1.4-1.4z"/></svg> 开发者信息

- **作者**：ZeroRaltal  
- **开源协议**：MIT  
- **项目地址**：[GitHub](https://github.com/Zero-Raltal/MCBE-CBNoteBlock)  
- **反馈建议**：欢迎提交 Issue 或 Pull Request。

## <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zM6 20V4h7v5h5v11H6z"/></svg> 许可证

本项目采用 [MIT 许可证](LICENSE)，允许自由使用、修改和分发，但需保留原作者声明。

---

*Made with <svg viewBox="0 0 24 24" width="1em" height="1em" fill="currentColor" style="display:inline;vertical-align:middle"><path d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"/></svg> by ZeroRaltal*
