# 可视化存档修改器

`index.html`、`editor.css` 和 `editor.js` 是界面源码。使用 Node 生成单文件 HTML：

```powershell
node tools/build-save-editor.mjs
```

产物为 `dist/web/save-editor.html`。独立打开时可编辑 Toolkit 阵型码、逐格布阵、撤销／重做并导出 JSON；支持六种场景以及植物、底座、南瓜、咖啡豆、梯子的叠层。

游戏网页在投币授权后，将这个产物嵌入同源的受限 `srcdoc` iframe。`pvz-save-editor-v1` 消息协议提供 `init`、`generate`、`extract`、`export`、`import`、`save-lineup`、`perfect` 和 `close` 操作。宿主检查消息来源、当前授权、游戏已经关闭及单任务状态；DAT64 的读取、校验和生成复用游戏自带的隔离 Wasm 存档工具。

HTML 不直接接触正在运行的关卡对象，不更改共享游戏代码或存档格式。独立页面没有 DAT64 引擎时会明确提示从游戏入口打开。用户资料可以通过宿主备份到云端，关卡 DAT64 只保存在本地；部署完美进度前必须确认删除现有用户及关卡存档。
