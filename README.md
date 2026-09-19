# ttp_i18n.dll

为 TTPlayer 重建版提供可选的 gettext 翻译支持。本目录保存 DLL 源码、翻译文件和提取工具，
通过 [`api.h`](include/ttplayer/i18n/api.h) 中带版本号的 C ABI 与播放器交互。
实现为项目自有代码，不依赖 `libintl.dll` 或 `libiconv.dll`。

## 构建

`gettext` 和 `rebuild` 分别构建、分别发布，通过版本化 C ABI 在运行时交互。
本仓库包含自己的接口头文件、兼容运行库配置、导入审计和依赖许可，可单独克隆构建。
在本仓库根目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release --target ttp_i18n ttp_i18n_catalog_tests --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

DLL 和翻译分别输出到 `build/Release/ttp_i18n.dll` 和 `build/Release/i18n`。
只修改翻译文件后，重新构建也会复制更新的文件。

如果使用包含 `gettext`、`rebuild` 两个目录的本地工作区，也可在工作区根目录执行：

```powershell
cmake -S gettext -B gettext/build -G "Visual Studio 18 2026" -A Win32
cmake --build gettext/build --config Release --target ttp_i18n
```

DLL 和翻译目录分别输出到 `gettext/build/Release/ttp_i18n.dll` 和
`gettext/build/Release/i18n`。

普通版和 XP／Win7 兼容版共用同一份 x86 `ttp_i18n.dll`。
DLL 始终使用 VC-LTL 的 XP 运行库和 YY-Thunks 系统 API 适配，
并自动检查 XP／Win7 导入兼容性，不需要分别维护两个版本。
播放器构建独立完成，不编译本仓库，也不复制本仓库的翻译文件。
DLL 的 Debug 配置也使用兼容运行库，保留调试符号；发布时使用 Release。

首次构建会下载固定版本并校验哈希的 VC-LTL 和 YY-Thunks，需要 MSVC、Win32 工具链
及 Python 3。审计报告输出为 `i18n-legacy-imports.json`，依赖许可输出到 `licenses`，
分发 DLL 时一并携带许可。

## GitHub Actions

在本仓库的 **Actions → Manual i18n Windows Build → Run workflow** 手动运行
[构建工作流](.github/workflows/manual-build.yml)。`configuration` 可选择
`Release`（默认）、`RelWithDebInfo` 或 `Debug`。

工作流使用 [GitHub 官方 Windows Server 2025／VS 2026 镜像](https://github.com/actions/runner-images#available-images)，
构建 x86 DLL，运行目录解析和简繁翻译测试，并检查 DLL 的 XP／Win7 静态导入。
ABI、兼容构建脚本、导入检查脚本和所需许可均随本仓库提供。

成功后，在该次运行的 **Artifacts** 下载 `ttp_i18n-Windows-x86-配置-运行编号`。
其中的 `ttp_i18n-x86-配置.zip` 包含：

- 两版播放器共用的 `ttp_i18n.dll`。
- `i18n` 下的简体、繁体、英文翻译及模板。
- XP／Win7 导入审计报告、构建信息、文件 SHA-256 清单。
- 使用说明、项目许可和兼容依赖许可。

ZIP 的 SHA-256 清单随产物提供；有调试符号时另附 PDB。产物保留 14 天，
失败时上传配置／测试诊断并保留 7 天。工作流仅需仓库读取权限，生成可下载构建产物。
导入检查用于验证加载依赖，旧系统上的实际行为仍需在对应系统中测试。

本地打包可在本仓库根目录执行：

```powershell
./tools/package.ps1 -BuildDirectory build -Configuration Release -Destination artifact
```

打包脚本会检查 DLL 与兼容审计报告的 SHA-256 一致，并检查翻译和依赖许可齐全。

## 部署与读取规则

将 DLL 和翻译目录放在播放器旁边，运行时仍使用 `i18n` 目录名：

```text
TTPlayerRebuild.exe
ttp_i18n.dll
ttpcomm.dll
ttpres.dll
i18n/
  chs/LC_MESSAGES/
    ttplayer.po          # 简体中文
  cht/LC_MESSAGES/
    ttplayer.po          # 繁體中文
  en_US/LC_MESSAGES/
    ttplayer.po
    ttplayer.mo          # 可选
```

在主窗口右键菜单的“界面语言”中选择语言，重启后生效。设置保存在
`TTPlayerRebuild.xml` 的 `General/@Language`：`auto` 跟随系统界面语言，
`source` 使用原始文本，也可指定 `chs`、`cht`、`en_US` 等语言标识。

简体中文的 `zh_CN`、`zh_SG`、`zh-Hans`、`zh_CHS` 会查找 `chs`；
繁体中文的 `zh_TW`、`zh_HK`、`zh_MO`、`zh-Hant`、`zh_CHT` 会查找 `cht`。
加载顺序为具体语言及其父级目录、`chs`／`cht` 别名目录、通用 `zh`；
显式脚本标识优先于地区，例如 `zh-Hant-CN` 使用 `cht`。
系统自动语言和旧配置中的 `zh_CN`／`zh_TW` 因此无需改名即可使用中文 PO。

同一语言存在有效的 `ttplayer.mo` 时优先读取 MO；MO 缺失或无效时读取 PO。
有效 MO 缺少某个词条时，不再读取同语言的 PO，而是尝试父语言，然后回退原文。
修改 PO 后应重新生成或移除旧 MO，并重启播放器。只有 PO 也能运行，无需安装 gettext 工具。

PO 使用 UTF-8，支持 BOM、多行字符串、C 转义、`msgctxt` 和复数形式；
MO 支持版本 0 的大小端格式。空译文、模糊（fuzzy）条目和废弃条目不参与翻译。
格式不兼容的译文也会回退。没有 DLL、DLL 接口不兼容或没有可用翻译时，
播放器继续使用 `ttpres.dll` 资源及重建版自建文本。

播放器直接读取 `ttpres.dll` 和 EXE 的字符串表、菜单及对话框原文，不再编译
`resource_messages.inc` 文本副本。DLL 加载 PO/MO 时为 `ttpres/`、`exe/` 下的
字符串、菜单和对话框词条建立资源上下文索引：优先精确匹配 `msgctxt + msgid`，
原文语言不同时使用唯一的资源上下文匹配。上下文对应多个有效原文时不猜测译文。
格式占位符、分隔符和过滤模式按当前加载资源校验；缺词或校验失败即显示该资源原文。
资源本身不存在时返回空文本。自建文本和复数仍按原有 gettext 键精确匹配。

接口继续兼容 ABI v1；部署时同时更新 EXE 和 DLL，可获得上述跨资源语言匹配能力。
旧 DLL 仍可加载，但它只能按实际资源原文精确查找，未命中时显示资源原文。

正常播放器启动时，先尝试加载 EXE 同目录的 `ttp_i18n.dll`，再主动加载并初始化
`ttpcomm.dll`。此时只读取配置中的语言并初始化翻译，完整设置和皮肤仍在后续阶段加载，
DLL 保持到会话结束。因此启动阶段的错误弹窗也可以使用所选语言。
私有插件工作进程沿用原有启动流程。

`MessageBox` 的正文和标题使用同一套翻译：资源提示按 `msgctxt` 查找，
代码中的固定提示使用 `app` 上下文。包含文件路径、曲目名或错误码的提示，
先翻译固定文本，再填入实际数据；缺少译文时继续显示原文。

XP／Win7 兼容版有一项例外：为保证 XP 上 `ttpcomm.dll` 的静态线程局部存储（TLS）正常，
EXE 保留了对它的启动导入，因此 Windows 会在入口函数运行前加载它。
该版本只保证程序主动加载阶段先处理 i18n，不能保证系统加载器实际先映射 `ttp_i18n.dll`。
`ttp_i18n.dll` 始终是可选组件，不会成为 EXE 的强制导入依赖。

## 维护翻译

翻译模板位于 [`i18n/ttplayer.pot`](i18n/ttplayer.pot)。语言文件分别为
[简体中文](i18n/chs/LC_MESSAGES/ttplayer.po)、[繁體中文](i18n/cht/LC_MESSAGES/ttplayer.po)
和 [English](i18n/en_US/LC_MESSAGES/ttplayer.po)。简繁中文已补齐当前模板；英文仍为部分翻译。
各 PO 按自建文本、重建版资源、`ttpres` 字符串表、菜单及对话框分节，使用相同的
`msgctxt` 和 `msgid`，便于维护同一套模板。资源原文语言差异由 DLL 的资源上下文索引处理。
新增语言采用相同目录结构。
请保留 `msgctxt`、格式占位符（如 `%d`、`%s`、`%(Title)`）、结构化字符串的
`|` 和换行分隔符，以及文件过滤模式。

从工作区根目录更新模板：

```powershell
python gettext/tools/extract_catalog.py
```

脚本扫描重建版代码，并复用现有 `gettext/i18n/ttplayer.pot` 中的资源词条，
将模板写回 POT，不生成主程序文本表。需要重新提取原始资源时，可传入
`--ttpres ttpres.dll --exe TTPlayer.exe`；提取二进制资源需要 Windows。
`--rebuild` 可指定重建版源码目录，`--output` 可指定模板输出文件，
`--resource-template` 可指定用于复用资源词条的 POT。构建 DLL 和播放器、运行播放器均无需 POT。

已有 `ttpres/chs/texts.json` 和 `ttpres/cht/texts.json` 导出文件时，可按资源 ID
补充新增字符串，并同步三个语言目录：

```powershell
python gettext/tools/extract_catalog.py --resource-texts ttpres/chs/texts.json --resource-texts ttpres/cht/texts.json
python gettext/tools/sync_catalogs.py
python gettext/tools/sync_catalogs.py --check
```

`--resource-texts` 只补充缺失的字符串表 ID，保持已有资源原文稳定。本次繁体资源中
独有的 `ttpres/string/136` 更新提示以繁体原文作为 `msgid`，简体 PO 提供对应译文。
重新从 DLL 提取资源时也应带上这两个参数，以保留另一个版本新增的字符串。

同步脚本按上下文匹配 `ttpres` 导出的菜单、对话框和字符串表，只填充空译文，
保留已有人工修改和模糊标记，将退出模板的条目保留为废弃条目。
字体、控件实现标签、URL、版本信息及嵌入文件不纳入界面翻译。
重建版自建文本直接在 PO 中维护。繁体资源的一处格式说明保留了原文占位符
`%(字段名)`，其余内容采用提供的繁体资源文本。若当前资源将此占位符写为其他文字，
该条会因格式不匹配而回退为当前资源中的说明。
`--check` 只检查同步状态，不写入文件；脚本不生成 MO。

如已安装 GNU gettext 工具，可在工作区根目录生成 MO：

```powershell
msgfmt --check --check-format -o gettext/i18n/en_US/LC_MESSAGES/ttplayer.mo gettext/i18n/en_US/LC_MESSAGES/ttplayer.po
```

## 验证

本仓库构建时启用 `BUILD_TESTING=ON`，然后运行翻译解析和目录校验：

```powershell
cmake --build build --config Release --target ttp_i18n ttp_i18n_catalog_tests --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

在含两个仓库的本地工作区中，需要播放器界面和启动弹窗集成测试时，可将已经构建好的
DLL 绝对路径传入播放器的 `TTPLAYER_I18N_TEST_DLL`，再构建和运行播放器测试：

```powershell
cmake -S rebuild -B rebuild/build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=ON -DTTPLAYER_STAGE_RUNTIME=OFF "-DTTPLAYER_I18N_TEST_DLL=$((Resolve-Path gettext/build/Release/ttp_i18n.dll).Path)"
cmake --build rebuild/build --config Release --target i18n_ui_tests --parallel 4
ctest --test-dir rebuild/build -C Release -R "^(i18n_ui_tests|i18n_startup_tests)$" --output-on-failure
```

翻译校验会检查简繁中文的模板覆盖、占位符、分隔符及过滤模式，并验证资源导入和
重复同步不会覆盖人工修改。独立执行：`python gettext/tests/translation_tests.py`。
