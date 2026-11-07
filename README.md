# PvzDevTools

[pvz_perf](https://github.com/YuHao2006/pvz_perf) 的独立开源开发工具工程。工具源码、资源构建链和后续的 reanim／particle 编辑工具在这里维护；游戏仓库只分发已构建的工具程序。

| 程序 | 用途 |
| --- | --- |
| `reanimc.exe` | reanim、particle、projectile DSL 编译、轨道枚举和字符串代码生成、MaxRects 可行布局 |
| `pvz-resource-build.exe` | 资源配置、字体、原生 CP-SAT 图集求解、PNG 合成、静态 C++ 数据、加载绑定、PAK64 打包和可选重压缩 |
| `pvz_dat64_converter.exe` | 原版 1073 DAT、DAT64 v1／v2／v4 及用户资料转换 |

正式资源构建链全部使用 C++23，不启动 Python。测试、输出对拍和性能采集可以使用 Python，不属于游戏运行时依赖。Windows 发布包内附求解器所需的 DLL；它们仅供开发工具使用，不进入游戏资源包或 APK。

资源、DSL、枚举顺序、图集配置和布局缓存属于游戏项目，不放进工具仓库。转换器在 `saveConverter/contracts` 冻结所支持版本的格式契约，修改游戏存档格式时必须明确更新契约和测试。工具版本号独立于游戏版本号。

## 构建

需要 CMake 3.24+、Ninja、C++23 编译器；Windows 使用 MSVC x64。依赖为 zlib、libspng、libjpeg-turbo、nlohmann-json 和 **OR-Tools 9.15.6755**。固定求解器版本用于复用现有证明缓存；不同求解器版本不得冒用旧证明。

从 VS x64 开发者终端构建。例如用 vcpkg 提供图像和 JSON 依赖，并通过 `CMAKE_PREFIX_PATH` 指向 OR-Tools C++ 发行包：

```powershell
cmake -S . -B out/windows-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DCMAKE_PREFIX_PATH=C:/DevTools/or-tools
cmake --build out/windows-x64
pwsh tools/package-windows.ps1 -BuildDirectory out/windows-x64
```

OR-Tools 的官方预编译包见 [官方 C++ 安装说明](https://developers.google.com/optimization/install/cpp/binary_windows)。不需要 Android Studio 或游戏的图形 SDK 来构建这些主机工具。

## 构建游戏资源

```powershell
./pvz-resource-build.exe build --root C:/src/pvz_perf `
  --compiler ./reanimc.exe --cache C:/src/pvz_perf/out/cache/resources
```

默认读取游戏的 `assets/definitions/ResourceBuild.pvzbuild`，向 `pvzPerf/Sexy.TodLib/generated` 写入生成文件。`--config` 和 `--generated-dir` 可覆盖输入和生成目录，便于在独立目录对拍。

图集不旋转，不缩放单图，保留两像素 RGBA 扩边。沿用配置中的每图集预算和最优要求：`FEASIBLE`／`UNKNOWN`、界和 gap 必须如实记录；只有经过证明的结果可以标为最优。保留更好布局、有效证明和未完成检查点；失败时不发布半成品资源包。图片 PNG 的编码字节可以不同，RGBA 像素必须保持无损一致。

装箱的宽、高硬上限均为 **4096**，配置不能提高此上限。原版超过上限的字体长条不经过装箱求解，构建器会在字形边界折行并更新字体矩形；字形像素、字宽、字距和偏移保持一致，不在加载期缩图。旧缓存必须重新校验四千零九十六上限；来自更宽搜索域的合法最优解仍然最优，已耗尽预算的可行解仍只标为 FEASIBLE／UNKNOWN，并记录原搜索域与来源键。超限布局不能复用。

可选重压缩输出独立的 `main-compact.pak64`，保留原包和源素材：

```powershell
./pvz-resource-build.exe compact --input main.pak64 --output main-compact.pak64 --mode lossless
```

`light-lossy` 仅允许图集 RGB 误差不超过 2/255，不改变透明度、尺寸或 PNG 元数据。普通图片、字体和透明遮罩保持无损。

转换器的完整命令见 `pvz_dat64_converter.exe --help`。未发布的 DAT64 v3 中间格式不支持，工具拆分没有改变 v4 存档格式。

转换器同时附带于游戏的 Win7 发行包，因此 Windows 发布时用仍支持 Win7 的 MSVC 14.44 单独构建 `saveConverter/src`，并检查硬导入；用 `package-windows.ps1 -ConverterExecutable <旧工具链产物>` 指定这份程序。主机资源构建器和 OR-Tools 运行库不要求在 Win7 上运行。

## 可视化存档修改器

`saveEditor/` 提供 HTML 存档修改界面，使用 `node tools/build-save-editor.mjs` 生成单文件产物。支持 Toolkit 阵型码、六场景逐格布阵、叠层、撤销／重做、无尽参数，以及通过游戏宿主完成 DAT64 校验、导入、导出和完美进度部署。它不注入运行中的游戏。接口与独立使用范围见 [saveEditor/README.md](saveEditor/README.md)。

## 测试与验证

游戏资源与测试夹具不随本仓库重复分发。针对同级 `PlantsVsZombiesModern` 或通过 `PVZ_GAME_ROOT` 指定的游戏工程运行测试。当前 C++ 构建器与冻结 Python 实现对拍；Python 实现只保留为测试参照，不能作为生产构建回退路径。

首次迁移验证涵盖 746 个资源包条目、20 份字体、106 份 particle 运行时记录、144 个 reanim 静态定义以及所有生成绑定。Windows Release 发布经校验的二进制和校验和；暂不发布 Linux／macOS 工具二进制。

代码延续原项目 CC0 许可；第三方库仍遵循各自许可。这里不重新授权原游戏的资源。
