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
| `Content/Project/` | Character、Interaction、AI、Online、World、Shared 等正式功能资源 | ✅ |
| `Content/zxx/BMMCSystem/` | 受保护模板与地图，全部原位保留 | ✅ |
| `Content/` 的素材包目录 | 现有素材原件与演示内容；正式关卡资源集中在 Project | ✅ |
| `Content/Developers/<个人名>/` | 个人沙盒（磁盘保留，仅本机使用） | ❌ |
| `Plugins/ECPFoundation/` | 普通常驻插件：公共契约、GameInstance、统一功能注册入口 | ✅ |
| `Plugins/GameFeatures/` | ECPCharacter、ECPInteraction、ECPAI、ECPOnline、ECPWorld 的 C++、描述及功能数据 | ✅ |
| `Plugins/FUOnlineSession/` | 自研 Steam/Null 会话插件（Source + Config） | ✅ |
| `docs/` | 团队规范文档 | ✅ |

项目采用功能插件与 Project 内容双层布局。C++ 与功能数据位于插件，蓝图和玩法资源位于所属 Project 目录。当前生产流程包含主菜单 Test_Map、原位保留的 BMMC 联机大厅与 Project/World/Maps/Demo。旧迁移备份和部分旧路径已在后续整理中移除；原生反射 CoreRedirects 继续保留。

> 让一个已跟踪的目录"出库"只有一条路：`git rm -r --cached <路径>`。
> 改 `.gitignore` 对已跟踪文件无效——`Content/Developers` 曾因此"文档说不入库、实际入库 171 个"。
> BMMC 生产大厅必须继续入库，不适用个人沙盒的出库规则。

## 当前开发与验证记录（docs/）

| 文档 | 内容 |
|---|---|
| [GameFeature 设计](docs/superpowers/specs/2026-09-23-game-features-design.md) | 模块职责、依赖和统一启动入口；历史迁移数量不代表当前资源清单 |
| [拾取、负重与布娃娃搬运](docs/superpowers/specs/2026-10-09-unified-pickup-load-ragdoll-carry-design.md) | 交互与公共角色规则 |
| [Project 内容整理](docs/project-content-cleanup-2026-10-12.md) | 三张生产关卡、环境资源与引用整理 |
| [多人切图](docs/level-travel-2026-10-12.md) | BP_Actor 服务器切图接口与真实双端 PIE 验证 |

## CI

push 到 `main` / `ZANEN` 或 PR 到 `main` 时自动编译 Editor + Game 两个目标，
需要带 `self-hosted`、`windows`、`ue5` 标签的自托管 Windows runner，安装 UE 5.8、Git LFS 和项目所需编辑器插件，并通过 `UE_ROOT` 环境变量提供引擎目录。

## 改资产的规矩（速记）

1. **先锁再改**：例如 `git lfs lock Content/Project/Interaction/Player/BP_ECPlayertest.uasset`；编辑、保存并检查差异，提交和交接后释放同一文件的锁。逐个锁定实际修改的文件，不用通配符锁目录；只读保存报错不能当作成功。
2. 新增正式地图放 `Content/Project/World`，不得依赖不入库的个人沙盒。当前默认地图为 `Project/World/Maps/LegacyZxx/Test_Map`，BMMC 原位地图继续保留。
3. 保留对应 CoreRedirects；禁止全局 Fix Up Redirectors，不为清理引用批量重存 BMMC 模板或其他引用方。整理资源前先核对当前引用与备份。
4. 新玩法资源写入所属 `Content/Project/<功能>/`；移动或改名必须通过编辑器资产接口核对引用和保存范围，不能直接在文件系统里移动二进制包。
5. GameFeature 与 Project 内容迁移已执行，专用迁移工具不能重复作为日常整理命令使用。当前资源整理和恢复说明见上述 Project 内容整理记录。
