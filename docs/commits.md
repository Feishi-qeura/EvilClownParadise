# 提交规范（Conventional Commits）

历史里有 `111`、"需要切换分支，所以先提交了" 这类提交——它们让 `git log`/`git bisect`/自动变更日志全部失效。本规范自本文件入库起生效。

## 格式

```
<type>(<scope>): <一句话描述，动词开头>

[可选正文：动机、影响范围、需要 reviewer 注意什么]
```

- `type`：`feat` 新功能 / `fix` 修复 / `refactor` 重构 / `perf` 性能 / `docs` 文档 / `chore` 工具与杂项 / `build` 构建与依赖 / `test` 测试 / `asset` 资产变更（内容）
- `scope`：模块名，如 `3c`、`ai`、`session`、`config`
- 描述用中文即可，一行说清"做了什么"，正文说清"为什么"

## 示例（按本仓库真实历史改写）

```
feat(ai): 新增 StateTree 巡逻/追击/攻击任务，怪物参数开放给任务读取

fix(3c): Enhanced Input 松开键不补发零值 Triggered，StopMove 手动归零 MoveInput

asset(3c): 重存 ABP_TestRobot 与 BS_Locomotion，引用改用 _Game 下的骨架

refactor(session): 把租约结果收敛进状态机，诊断输出加防重入保护
```

## 红线

1. 禁止 `111`、`tmp`、`wip` 等无信息提交标题。
2. 不为切分支而提交；用 stash（`git stash push -m "原因"`）。
3. PR 改动了二进制资产时，在 PR 描述里**列出资产路径**——reviewer 用它判断是否需要锁与回归测试。
4. 一个提交只做一件事：修 bug 不顺手加功能；资产重存不混代码改动（除非必要，分开提交）。
