# C++ 代码规范

> 基准：[Epic 官方 Coding Standard](https://dev.epicgames.com/documentation/zh-cn/unreal-engine/epic-c-plus-plus-coding-standard-in-unreal-engine)。
> 本文件只写"Epic 标准之上的项目约定"。仓库已内置 `.clang-format` 与 `.editorconfig`，格式交给工具，人不手改。

## 命名与结构

1. 前缀 `ECP`（类 `AECPMonsterBase`，文件同名）；模板/委托用 `FECP`，枚举 `EECP`，结构 `FECP`。
2. 头文件放 `Public/<分类>/`，私有实现放 `Private/<分类>/`，目录名用语义分类（`Characters`、`AI`、`Camera`、`Data`…）。新目录归类拿不准就先问。
3. 类别（Category）统一 `ECP|主题`（现有惯例：`ECP|Health`、`ECP|3C`、`ECP|AI`），蓝图侧查找靠它过滤。
4. 日志用 `DEFINE_LOG_CATEGORY_STATIC(LogECPxxx, ...)`，不要裸用 `LogTemp`。

## 写法要点

1. **注释写"为什么"**：现在的代码注释是加分项，保持这个风格——说明坑在哪（如 Enhanced Input 松键不补发零值、移动夹取的原因），不要复述代码在干什么。
2. 指针解引用不要写 `ptr -> member`（有空格），工具已统一。
3. 局部变量驼峰（`AttackInterval`、`now` → `TimeNow`），不要随手小写。
4. 空函数不留空壳：`ECPGameMode` 这类"先占位"的类，要么写上意图注释，要么先不建文件。
5. 裸帮助函数注意空指针：`DebugHelper::Print` 这类全局辅助必须先判 `GEngine`，编辑器命令行环境会崩。
6. 服务器/客户端语义写进注释：哪些函数 `HasAuthority()` 才能跑、哪些 UPROPERTY 需要 Replicated，写明。

## 流程

1. 改代码后本机编译通过再提交；CI 会再编译一遍双目标（Editor + Game）。
2. 改类名/结构名时：C++ 改名 → 编辑器里 Fix Up Redirectors → 资产重存 → **删除** `DefaultEngine.ini` 里对应 CoreRedirects，redirect 不允许长住。
