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

用 Rider/VS 打开 `EvilClownParadise.uproject`，编译 `EvilClownParadiseEditor` 目标后启动编辑器。

> 二进制资产走 LFS：clone/切换分支若提示 `smudge filter lfs failed`，先执行 `git lfs install` 再 `git lfs pull`。

## 目录约定

| 目录 | 内容 | 是否入库 |
|---|---|---|
| `Content/_Game/` | 游戏正式资产 | ✅ |
| `Content/Developers/<个人名>/` | 个人沙盒（磁盘保留，仅本机使用） | ❌ |
| `Plugins/FUOnlineSession/` | 自研 Steam/Null 会话插件（Source + Config） | ✅ |
| `docs/` | 团队规范文档 | ✅ |

学习/演示素材不入库，且已从磁盘移出；完整副本在 `E:\UnrealProjects\ECP_Archive`（仅本地保留）。

> 让一个已跟踪的目录"出库"只有一条路：`git rm -r --cached <路径>`。
> 改 `.gitignore` 对已跟踪文件无效——`Content/Developers` 曾因此"文档说不入库、实际入库 171 个"。

## 团队规范（docs/）

| 文档 | 内容 |
|---|---|
| [docs/branching.md](docs/branching.md) | 分支模型：功能分支短命合并，禁止按人开长活分支 |
| [docs/commits.md](docs/commits.md) | 提交规范：Conventional Commits，禁止无信息提交 |
| [docs/code-style.md](docs/code-style.md) | C++ 规范：Epic 标准之上的项目约定 |
| [docs/content.md](docs/content.md) | Content 目录结构、资产命名、资产协作与批量迁移的正确做法 |
| [docs/ci.md](docs/ci.md) | 自托管 runner 与 CI 设置 |
| [docs/tech-debt.md](docs/tech-debt.md) | 技术债与后续任务清单（完成请勾选） |

## CI

push 到 `main` / `ZANEN` 或 PR 到 `main` 时自动编译 Editor + Game 两个目标，
需要一台自托管 Windows runner（设置见 [docs/ci.md](docs/ci.md)）。

## 改资产的规矩（速记）

1. **锁再改**：`git lfs lock <路径>` → 编辑 → `git lfs unlock <路径>`。
   这不是自觉要求：`.gitattributes` 标了 `lockable`，**Git LFS 会把未加锁的 `.uasset`/`.umap`
   在工作区设为只读**，不加锁保存会报 `Cannot remove '...' as it is read only!` 且改动静默丢失。
2. 启动地图只允许指向 `Content/_Game`，不得指向个人沙盒——那不入库，同事拉库必报错。
3. 类改名 = C++ 改名 + 编辑器 Fix Up Redirectors + 重存资产 + **删除对应的 CoreRedirects**。
4. 关闭编辑器再对 Content 目录做移动 / 删除类操作。
5. 迁移被引用的资产不要用脚本 `rename_asset` / `consolidate_assets`，会静默损坏引用；
   正确做法见 [docs/content.md](docs/content.md) 的"批量迁移被引用的资产"。
