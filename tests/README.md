# 工具回归

`verify_resource_migration.py` 比较完整资源包、字体与 NRSC4 字节、静态 C++ 输出和挂接掩码，并实际执行原生 CP-SAT 搜索、验证界和缓存复用。输入为本地游戏工程、参考 PAK、原生工具和一次独立构建的输出目录。

`verify_compact_output.py --builder <程序>` 验证两种 PNG 重压缩策略、透明度、元数据、资源闭包和失败时保留旧输出。

转换器的三组 C++ 回归通过 `-DPVZ_TEST_FIXTURE_DIR=<游戏工程>/Tests` 启用，使用 CTest 执行。测试实现可以包含转换器的 `.cpp` 来访问内部解析器，不增加正式工具 API。

`reference/python` 是迁移前算法的冻结参照，版权和原始文件 SHA 见 `SOURCE_ORIGIN.json`。它仅用于对拍，不会被生产构建器调用或进入 Windows 二进制发布包。`PVZ_GAME_ROOT` 指定资源和游戏测试夹具所在工程；默认使用同级 `PlantsVsZombiesModern`。
