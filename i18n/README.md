# 界面翻译

把本目录放在 `TTPlayerRebuild.exe` 旁边，将 `ttp_i18n.dll` 放入播放器的 `AddIn` 目录，在主窗口右键菜单
“界面语言”中选择语言，重启生效。“原始文本”可恢复原来的界面文字。

每种语言使用 `语言标识/ttplayer.po`：

| 目录 | 语言 | 内容 |
| --- | --- | --- |
| `chs/ttplayer.po` | 简体中文 | `ttpres/chs` 资源译文及重建版自建文本 |
| `cht/ttplayer.po` | 繁體中文 | `ttpres/cht` 资源译文及重建版自建文本 |
| `en_US/ttplayer.po` | English | 部分翻译，缺词显示原文 |

PO 使用 UTF-8；`ttplayer.pot` 是完整翻译模板。各 PO 内按自建文本、
重建版资源、`ttpres` 字符串表、菜单和对话框分节。

`zh_CN`、`zh_SG`、`zh-Hans` 可读取 `chs`；`zh_TW`、`zh_HK`、`zh_MO`、
`zh-Hant` 可读取 `cht`，因此自动跟随系统语言时也能使用这两个目录。
若同时提供了具体地区的翻译，先使用具体地区及其父级目录，再使用 `chs`／`cht`，
最后尝试通用 `zh`。显式脚本标识优先于地区，例如 `zh-Hant-CN` 使用繁体。

同目录存在有效 `ttplayer.mo` 时优先使用 MO；MO 缺失或损坏时读取 PO。
修改 PO 后，需要删除旧 MO 或重新编译 MO，再重启播放器：

```powershell
msgfmt --check --check-format -o en_US/ttplayer.mo en_US/ttplayer.po
```

只有 PO 也可以运行，无需安装 gettext 工具。空译文、模糊（fuzzy）条目和
格式不兼容的译文会回退。翻译时保留 `%d`、`%s`、`%(Title)` 等占位符、
结构化字符串的 `|` 和换行分隔符，以及文件过滤模式。

没有 `ttp_i18n.dll` 时，播放器沿用 `ttpres.dll` 和重建版自带文本。

资源提取、补全和分类方法见 [维护翻译](../README.md#维护翻译)。
