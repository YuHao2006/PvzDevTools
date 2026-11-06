# reanimC — reanim 编译器（可独立构建）

从 openPvZ 主树提取的可独立编译布局，**修改请回主树**（`tools/reanimc/`、`src/reanim/`）。

## 构建（Ninja + MSVC x64）

```bat
build.bat
:: 产物 build\reanimc.exe
```

## 布局

| 文件 | 来源 | 内容 |
|---|---|---|
| `src/main.cpp` | `tools/reanimc/src/main.cpp` | CLI：`import`（Legacy reanim XML → DSL）、`compile`（DSL → pak64 记录）、`compile-v2`（语料级编译：轨道名 → uint32 id 内嵌 + **IDT3 attacher 段表**，cookie DEADFED6）、`inspect` |
| `src/Pak64.cpp` + `include/reanim/Pak64.h` | `src/reanim/` | 编译记录容器（RawReanim 读写、pak64 封装） |

## 性能论证时看什么

- `compile-v2` 的 attacher text 解析与段折叠逻辑（**已知 bug 现场就这里**：
  折叠边界处理导致段表空窗（乐队消失）与跨空 text 过度延伸（军团附件链式
  爆炸）；正确语义 = 按值比较文本、空 text 终止段并显式产出 NONE 段）；
- `stable_id`（主树 `tools/common/include/openpvz/anim/`）——名称→ID 的编译期边界；
- `Pak64.h` 的记录内存布局（连续 transform 数组 / 轨道区间）。
