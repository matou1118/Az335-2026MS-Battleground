# 码头哥 2026 MS 战场版

> 官网 / Official site: https://ttread.cn

> **Matou 2026 MS Battleground Edition**
> 面向《魔兽世界》3.3.5 (巫妖王之怒) 的 AI 机器人服务器端增强版本

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![禁止商用](https://img.shields.io/badge/%E7%A6%81%E6%AD%A2%E5%95%86%E7%94%A8-red.svg)](docs/DISCLAIMER.md)

> ⚠️ **本项目仅供学习、开发与技术交流，禁止商业用途。**
> 请先阅读 [`docs/DISCLAIMER.md`](docs/DISCLAIMER.md)。

**📖 完整文档：** https://github.com/matou1118/Matou2026MS-Battleground

---

## 这是什么

本仓库是 **AzerothCore + mod-playerbots** 的完整源码树，
针对**战场（Battleground）**场景做了深度强化。

与原版 mod-playerbots 的区别：

| 方面 | 原版 | 本项目 |
|---|---|---|
| 战场战术 | 通用脚本 | 动态回防、**GUID 稳定岗位分配**、主动接战 |
| 职业 AoE | 部分缺失 | 8 个职业补齐 |
| 猎人宠物 | 动作已注册但**无策略引用** | 完整接线，含驯兽核心天赋 |
| 启动性能 | 22.4 秒空转 | 空输入提前返回 |
| 数据完整性 | 依赖用户自行导入 | 提供校验与修复工具 |

---

## 上游基线

| 组件 | 上游仓库 | 基线提交 |
|---|---|---|
| AzerothCore (fork) | `mod-playerbots/azerothcore-wotlk` | `2a2211cd` (2026-10-09) |
| mod-playerbots | `mod-playerbots/mod-playerbots` | `037c0141` (2026-10-02) |

**本项目改动**：核心 10 文件 / +38 −13，mod-playerbots 23 文件 / +651 −76。
改动明细见配套文档仓库的 [`CHANGELOG.md`](https://github.com/matou1118/Matou2026MS-Battleground/blob/main/CHANGELOG.md)。

> 本仓库以**单提交快照**形式发布（上游为浅克隆，完整历史不可用）。
> 如需叠加到自己的上游分支，请使用配套文档仓库提供的两个 patch 文件。

---

## 快速开始

> ⚠️ **前置条件**：你必须**自行持有正版 WoW 3.3.5 客户端**。
> 本仓库不包含也不分发任何暴雪娱乐的数据文件。

### 1. 环境要求

| 项目 | 最低 | 推荐 |
|---|---|---|
| 操作系统 | Windows 10 x64 | Windows 11 x64 |
| 内存 | 8 GB | 16 GB |
| 硬盘 | 30 GB | 60 GB |
| 数据库 | MySQL 8.0 | MySQL 8.0 |
| 编译器 | Visual Studio 2019 v16.5 | VS 2022 v17.x |
| CMake | 3.20+ | 3.25+ |

> ⚠️ **编译并行度必须用 `-j 2`**。`-j 16` 会将可用内存压至 7 MB 并冻结机器。

### 2. 克隆

```bash
git clone --recursive https://github.com/matou1118/Az335-2026MS-Battleground.git
cd Az335-2026MS-Battleground
```

### 3. 编译

```bash
# Linux
./build.sh

# Windows
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DTOOLS=ON -DSCRIPTS=static
cmake --build build --config Release -j 2
```

### 4. 提取客户端数据

将正版客户端路径填入 `build/bin/worldserver.conf` 的 `DataDir`，然后运行：

```
build/bin/map_extractor.exe        # -> data/maps
build/bin/vmap4_extractor.exe      # -> data/vmaps
build/bin/mmaps_generator.exe      # -> data/mmaps   （耗时数小时）
```

DBC 文件需用 `dbc2csv` / `DBCConvert` 等工具从客户端提取后放入 `data/dbc/`。

### 5. 初始化数据库

```bash
mysql -u root -p < data/sql/base/db_world/*.sql
mysql -u root -p < data/sql/base/db_auth/*.sql
mysql -u root -p < data/sql/base/db_characters/*.sql
```

### 6. ⚠️ 数据完整性校验（**必做**）

**这是本项目修复量最大的部分。** 上游建库流程在很多环境下未完整执行，
导致关键表严重残缺，且**残缺的数据库表会覆盖正确的 DBC 文件**。

```bash
# spell_dbc 正常应约 5 万行，若只有几千行则必然出问题
mysql -u root -p -e "SELECT COUNT(*) FROM acore_world.spell_dbc;"

# lfgdungeons_dbc 正常应为 195 行
mysql -u root -p -e "SELECT COUNT(*) FROM acore_world.lfgdungeons_dbc;"
```

**若数量异常，使用本仓库提供的工具修复：**

```bash
# 从正版客户端提取的 Spell.dbc + 列元数据
python scripts/dbc_import.py spell <路径>/Spell.dbc spell.sql spellmeta.tsv
mysql -u root -p acore_world < spell.sql

# 为缺少护甲的 bot 分配职业适配装备
python scripts/gear_up_naked_bots.py chars.tsv gear.tsv gearup.sql <起始实例GUID> ench.txt
```

> `dbc_import.py` 的字段布局**自动从本仓库的 `src/server/shared/DataStores/DBCfmt.h` 读取**，
> 不硬编码格式串，因此不会出现手抄 234 字符格式串错位的问题。
>
> 详见 [`scripts/dbc_import.py`](scripts/dbc_import.py) 与
> [TROUBLESHOOTING](https://github.com/matou1118/Matou2026MS-Battleground/blob/main/docs/TROUBLESHOOTING.md)。

### 7. 部署炉石脚本

```bash
cp lua_scripts/码头哥炉石.lua <你的服务端目录>/lua_scripts/
```

然后重启或执行 `.reload eluna`。

主菜单底部会显示版本标识「码头哥 2026 MS 战场版」。

### 8. 配置

从 `.conf.dist` 复制出实际配置：

```bash
cp configs/worldserver.conf.dist configs/worldserver.conf
cp configs/modules/playerbots.conf.dist configs/modules/playerbots.conf
```

按需修改的常用项：

```ini
LoginDatabaseInfo     = "127.0.0.1;root;root;acore_auth"
WorldDatabaseInfo     = "127.0.0.1;root;root;acore_world"
CharacterDatabaseInfo = "127.0.0.1;root;root;acore_characters"

DataDir = "/path/to/your/WoW/Data"

AiPlayerbot.MaxAddedBots       = 40
AiPlayerbot.AddClassCommand    = 1
AiPlayerbot.RandomBotJoinLfg   = 0    # bot 不自主排随机副本
```

### 9. 启动

**启动顺序很重要：**

```
MySQL  →  worldserver  →  等待 8085 端口  →  authserver
```

理由：`Main.cpp` 会在 authserver 启动前将 realm 标记为 offline，
顺序错误会导致客户端无法连接。

---

## 常用命令

| 命令 | 说明 |
|---|---|
| `.playerbots bot addclass <职业>` | 招募该职业 bot（一次一个职业）|
| `.playerbots bot randomize` | 随机化 bot |
| `.playerbots bot remove` | 移除 bot |
| `.dc on` | 副本难度切换（**需队伍内有 bot 坦克且在副本内**）|

> 注意：**没有** `.addclass` 这条命令，正确的是 `.playerbots bot addclass`。

---

## 本仓库新增的目录

```
.
├── lua_scripts/          传送炉石脚本（需复制到服务端 lua_scripts/）
│   ├── 码头哥炉石.lua
│   └── 天蓝超级炉石自用 微调2.0版.lua.ORIGINAL.bak   原始版本，供对照
├── scripts/              数据修复工具
│   ├── dbc_import.py         DBC 解析导入
│   ├── gear_up_naked_bots.py 批量装备分配
│   └── restyle_hstone.py     炉石脚本配色规范化
└── docs/
    ├── DISCLAIMER.md     免责声明 + 非商用条款
    └── NOTICE.md         第三方组件授权声明
```

其余目录（`src/`、`modules/`、`data/`、`deps/` 等）均来自上游，结构未变。

---

## 已知问题

| 问题 | 状态 |
|---|---|
| 随机副本客户端列表为空 | 服务端数据完整（195 条），客户端 UI 不显示，待排查 |
| `item_template.spellid_*` 存在无效法术引用 | 历史残留告警，不影响功能 |
| 商业训练师不按职业过滤 | 沿用上游实现 |
| mod-playerbots 无正式版本 | 仅 `master` / `test-staging` 滚动分支，无 release tag |

---

## 文档

完整文档在配套仓库：**https://github.com/matou1118/Matou2026MS-Battleground**

| 文档 | 用途 |
|---|---|
| `ANNOUNCEMENT.md` | 公告版，可直接发群/发帖 |
| `RELEASE_NOTES.md` | 版本说明、升级步骤、破坏性变更 |
| `CHANGELOG.md` | 版本迭代技术记录，含根因分析 |
| `docs/TROUBLESHOOTING.md` | **常见问题排查（先看这个）** |
| `docs/ARCHITECTURE.md` | 战场 AI 设计、崩溃修复、性能根因 |
| `docs/PORTING.md` | 移植到 2.4.3 / TrinityCore / 4.x |

---

## 许可与合规

代码以 **GPLv2** 发布（见 [`LICENSE`](LICENSE)）。
第三方组件授权见 [`docs/NOTICE.md`](docs/NOTICE.md)。

**本仓库不包含任何暴雪娱乐的受版权保护内容：**

- `*.dbc`、`data/maps/`、`data/mmaps/`、`data/vmaps/`
- 客户端可执行文件与 MPQ / CASC 资源

使用者须自行持正版客户端提取上述数据。

**本项目仅供学习、开发与技术交流，禁止商业用途。**

> **关于该条款的效力**：GPLv2 序言明确写道
> "The act of running the Program is not restricted" ——
> 运行行为不在 GPLv2 管辖范围内，因此无法通过附加条款约束。
> 该声明的效力为作者意愿、伦理倡议与侵权主张依据。
> 生态中真正约束商业化的是暴雪娱乐的著作权与 EULA。
> 详见 [`docs/DISCLAIMER.md`](docs/DISCLAIMER.md) 第 4.2 节。

---

**码头哥 2026 MS 战场版** · 2026.10
