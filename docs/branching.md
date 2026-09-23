# 分支模型

> 原则：分支代表**功能**，不代表人。分支以天到周为生命周期，合并即删。

## 分支类型

| 分支 | 用途 | 生命周期 |
|---|---|---|
| `main` | 可运行基线，受保护 | 永久 |
| `feature/<模块>-<一句话>` | 功能开发 | ≤ 1 周，合并即删 |
| `fix/<问题-一句话>` | 缺陷修复 | ≤ 2 天 |

规则：

1. **禁止按人开长活分支**（如 `ZANEN`、`FEISHI`）——工作成果应随功能合并进 `main`，个人分支堆积会让 `main` 落后、合并冲突滚雪球（uasset 不可合并，冲突尤其昂贵）。
2. 合并一律走 PR（已在做，保持）+ 至少一人 review + CI 编译通过。
3. 改动**二进制资产**（uasset/umap）的分支在开分支前先在群里声明影响的资产，避免两个分支同时编辑同一资产——二进制无法合并，只能丢弃一边。
4. 开发中途要切分支：用 `git stash` 或本地临时提交后 `git reset --hard`，不要把 WIP 提交推到公共分支。

## 个人分支的清理进度

个人长活分支与 `main` 平行时，`main` 会落后、冲突滚雪球（uasset 不可合并，冲突尤其昂贵）。
清理方式：成果以功能为单位提 PR 合入 `main`，合并后删除远程与本地分支：

```bash
git push origin --delete <分支名>     # 远程
git branch -d <分支名>                # 本地（-d 会校验已合并）
```

**进度（2026-09-17）**

| 分支 | 状态 |
|---|---|
| `ZANEN` | ✅ 已删除。成果已全部并入 `main`（0 个独有提交） |
| `ZANEN_Test` | ✅ 已删除。独有工作为在线登录实验（`ECPOnlineLoginAsync.h/.cpp` 126 行、`WBP_TestSteam` UI、在线配置），经本人确认无用。**唯一留存副本是标签 `archive/zanen-test-online`**，确认永久不需要后可删 |
| `chore/repo-hardening` | ✅ 已删除。第二轮正规化改造的功能分支，已并入 `main`，从未推送远端 |
| `FEISHI` | ✅ **可安全删除**：已全部并入 `main`（0 个独有提交） |
| `SANC` | ✅ **可安全删除**：已全部并入 `main`（0 个独有提交） |
| `FEISHI_NULL` | ✅ **可安全删除**：唯一独有提交 `dbd61c9` 是**回退性**改动（删除 `FU_OnlineProviderStatusEvaluator.h`、19 行自动化测试，简化 subsystem，净 +14/-80），`main` 上已有更完整的版本，删除不丢有效工作 |

> 上表"可安全删除"的分支仍挂着，删不删由各自负责人定——`FEISHI`/`FEISHI_NULL`/`SANC`
> 是同事的工作分支，不由他人代删。核对命令见下方教训。

> **教训**：删除前务必用 `git rev-list --left-right --count main...<分支>` 与
> `git log main..<分支>` 确认独有提交。`ZANEN_Test` 的提交标题全是 `111`，
> 但内容是一整套在线登录实现——**标题是垃圾不代表内容是垃圾**。

后续：

1. 新工作开短命分支：`git checkout -b feature/3c-crouch-camera main`
2. `main` 开启保护：Settings → Branches → Add rule（require PR、require CI pass）。

## 与二进制资产相关的约定

- 修改共享关卡/共享蓝图前，先在群里认领；同一时间一个资产只有一个人编辑。
- 大改动优先拆分：新关卡用 World Partition；共享蓝图上的大改先加子蓝图/组件，不动别人的部分。
