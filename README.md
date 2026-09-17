<div align="center">

<img width="437" height="798" alt="EvilClownParadise" src="https://github.com/user-attachments/assets/acf0167d-9745-4ee8-b8e5-53e66b71c8c5" />

</div>

# Evil Clown Paradise

团队第一款商业向独立游戏（UE 5.8 / C++ / Win64 / Steam 在线）。本项目同时承担团队的技术积累：3C、StateTree AI、在线会话均以正式工程标准实现。

## 环境要求

| 组件 | 版本 |
|---|---|
| Unreal Engine | 5.8（非默认安装路径时需设 `UE_ROOT` 环境变量） |
| Git | ≥ 2.40，**必须安装并初始化 Git LFS** |
| IDE | Rider 或 Visual Studio 2022（含 Unreal 组件） |

## 快速开始

```bash
# 每台机器只需一次
git lfs install

git clone git@github.com:Feishi-qeura/EvilClownParadise.git
cd EvilClownParadise
```

用 Rider/VS 打开 `EvilClownParadise.uproject` 或仓库根目录的 `.sln`，编译 `EvilClownParadiseEditor` 目标后启动编辑器。

> 二进制资产走 LFS：clone/切换分支若提示 `smudge filter lfs failed`，先执行 `git lfs install` 再 `git lfs pull`。

## 目录约定

| 目录 | 内容 | 是否入库 |
|---|---|---|
| `Content/_Game/` | 游戏正式资产 | ✅ |
| `Content/Developers/<个人名>/` | 个人沙盒 | ❌ 仅本地 |
| `Plugins/FUOnlineSession/` | 自研 Steam/Null 会话插件（Source + Config） | ✅ |
| `docs/` | 团队规范文档 | ✅ |

学习与演示素材不入库。历史归档副本在 `E:\UnrealProjects\ECP_Archive`（仅本地保留）。

## 团队规范（docs/）

| 文档 | 内容 |
|---|---|
| [docs/branching.md](docs/branching.md) | 分支模型：功能分支短命合并，禁止按人开长活分支 |
| [docs/commits.md](docs/commits.md) | 提交规范：Conventional Commits，禁止无信息提交 |
| [docs/code-style.md](docs/code-style.md) | C++ 规范：Epic 标准之上的项目约定 |
| [docs/content.md](docs/content.md) | Content 目录结构、资产命名、`git lfs lock` 资产协作规则 |
| [docs/ci.md](docs/ci.md) | 自托管 runner 与 CI 设置 |
| [docs/tech-debt.md](docs/tech-debt.md) | 技术债与后续任务清单（完成请勾选） |

## CI

push 到 `main` / `ZANEN` 或 PR 到 `main` 时自动编译 Editor + Game 两个目标，需要一台自托管 Windows runner（设置见 docs/ci.md）。

## 修改公共资产的规矩（速记）

1. 编辑共享 uasset/umap **先锁**：`git lfs lock <路径>`。
2. 启动地图只允许指向 `Content/_Game`，不得指向个人沙盒。
3. 类改名 = C++ 改名 + 编辑器 Fix Up Redirectors + 删除 CoreRedirects。
4. 关闭编辑器再对 Content 目录做移动/删除类操作。
