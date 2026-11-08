# 独立网页存档工具（wasm32）

本目录与 `saveEditor/` 的网页工具在自己的 Worker 中运行。它们不链接游戏对象文件，不启动 `LawnApp`／`Board`，不读取游戏 Wasm、内存、MEMFS、PAK、纹理、音频或动画定义。网页宿主只传入存档字节，并在游戏停止时部署返回的结果。

JavaScript 处理界面、用户进度、阵型码、对象引用重映射和关卡构造；`LevelSaveConverter.cpp` 编译成独立 wasm32，负责冻结格式解析、完整校验、v4 序列化、原版转换及已知坏档修复。游戏使用 wasm32 或 memory64 均不改变工具的位数。

```powershell
node saveConverter/web/build.mjs
node tools/build-save-editor.mjs
python -m http.server 18105 --bind 127.0.0.1 --directory dist/web
```

- `http://127.0.0.1:18105/save-editor.html`：独立修改器。支持用户新增／删除／进度、完美用户、六场景布阵、DAT64 生成／导入／导出／提取和植物卡死修复。IndexedDB 使用工具自己的 `pvz_devtools_save_editor_v4` 数据库，与游戏存档隔离。
- `dist/web/save-converter.html`：可直接离线打开的格式转换页面，核心内嵌。
- `save-tool-core.js`：工具的 wasm32 核心，初始线性内存 16 MiB，按需增长。
- `save-tool-worker.js`／`save-tool-client.js`：独立任务入口。
- `save-model.js`／`save-prefab.js`／`save-generate.js`：工具记录模型及生成器。
- 冻结初始记录压缩后内嵌于 `save-generate.js`，没有额外的 `.json.gz` 请求。B 站会为 `.json.gz` 加上 `Content-Encoding: gzip`，浏览器自动解压后再手动解压会产生 `TypeError: Failed to fetch`；内嵌数据仅在工具 Worker 内解压一次。

初始状态数据的格式、来源及更新规则见 `../contracts/generation/README.md`。生成器直接组合序列化记录，重新分配对象 ID，重映射动画／附件／粒子引用，并重新构造空闲链。生成后的文件必须通过独立核心完整读取和重写验证；不会通过启动隐藏游戏来获得输出。

当前保持 DAT64 v4 兼容。支持原版 1073 与 DAT64 v4 双向转换、用户明细转换；v1～v3 不作为网页有效入口。v5 尚未定稿，不修改版本头冒充新格式。原版不能表示任意纳秒，导出时可明确允许向原版时间单位舍入。

修复仅识别已知玉米炮攻击收尾卡死的完整状态组合，另存修复副本，不在游戏运行时猜测或修复原档。

验证脚本：`tests/save-converter-web.mjs`、`tests/save-engine-web.mjs`、`tests/save-generator-web.mjs`、`tests/save-generator-contracts.mjs`、`tests/save-editor-standalone.mjs`。用 `PVZ_PLAYWRIGHT_MODULE` 指定 Playwright。覆盖原版逐字节往返、用户档往返、六场景、多头／睡眠／底座／粒子引用、全部可用模式、独立修改器持久化，以及禁用游戏 Module／图形／音频后的操作。
