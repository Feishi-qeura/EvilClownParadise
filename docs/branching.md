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

## 现存分支的迁移（需要各自执行）

当前 `FEISHI`、`FEISHI_NULL`、`ZANEN`、`ZANEN_Test`、`SANC` 与 `main` 平行。迁移方式：

1. 把各自分支的成果以功能为单位提 PR 合入 `main`（一次功能一个 PR，便于 review）。
2. 合并后删除远程个人分支，例如：

```bash
git push origin --delete ZANEN_Test
git push origin --delete FEISHI_NULL
git push origin --delete SANC        # 若已合并
git branch -d ZANEN_Test             # 本地
```

3. 新工作开短命分支：`git checkout -b feature/3c-crouch-camera main`
4. `main` 建议开启保护：Settings → Branches → Add rule（require PR、require CI pass）。

## 与二进制资产相关的约定

- 修改共享关卡/共享蓝图前，先在群里认领；同一时间一个资产只有一个人编辑。
- 大改动优先拆分：新关卡用 World Partition；共享蓝图上的大改先加子蓝图/组件，不动别人的部分。
