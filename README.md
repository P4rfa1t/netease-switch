# netease-switch

网易云音乐的非官方 Nintendo Switch 原生客户端，用 libnx + SDL2 从零实现，不依赖 Android 或任何运行时。

> **免责声明**：本项目与网易、任天堂均无关联，不分发网易云官方 APK、图标或任何版权内容。
> 它通过音乐网页端使用的非公开接口访问用户自己的账号，仅供个人学习与自用。接口随时可能变更或失效。

## 功能

- 网易云 App 扫码登录，Cookie 保存在 SD 卡，下次启动自动登录
- 每日推荐、我的歌单、歌单详情、歌曲搜索
- MP3 / FLAC 播放（`dr_mp3` / `dr_flac`），暂停、切歌、进度跳转、音量调节
- LRC 歌词滚动显示
- 全手柄操作，适配底座与掌机模式

## 安装

1. 一台已运行 Atmosphère 等自制固件的 Switch（能启动 `.nro`）。
2. 自行构建（见下文「构建」），或从 GitHub Actions 的构建产物下载 `netease-switch.nro`，放到 `sd:/switch/`。
3. 在 hbmenu（相册）里启动。
4. 首次启动会显示二维码，用手机网易云音乐 App 扫码并在手机上确认。

运行数据（Cookie、日志）保存在 `sd:/switch/netease-switch/`：

| 文件 | 用途 |
| --- | --- |
| `cookies.json` | 登录 Cookie，删除即可退出登录 |
| `last.log` | 最近一次运行的日志，反馈问题时请附上 |

## 操作

| 按键 | 功能 |
| --- | --- |
| 十字键 / 摇杆 | 列表移动、切换标签 |
| A | 确认 / 播放 |
| B | 返回 / 关闭歌词 |
| X | 播放 / 暂停 |
| Y | 显示或隐藏歌词 |
| L / R | 上一首 / 下一首 |
| ZL / ZR | 音量 -/+ |
| + | 刷新当前列表 |

歌单页里左右方向键是 ±10 秒跳转。

## 关于 NSP

可以生成，但**不能直接靠 `make` 得到可用的安装包**：

- devkitPro 模板里的 `.nsp` 规则（`make CONFIG_JSON=...`）只是把 `main` + `main.npdm` 打成一个 ExeFS PFS0，没有 control NCA、图标和 RomFS，官方注释也写明它是给 sysmodule 用的。
- 真正能从 HOME 菜单启动的 homebrew NSP 需要用 `hacBrewPack` / `hacPack` 把 NRO 重新打成 Program / Control / Meta NCA，而这类工具需要主机的 `prod.keys`（版权密钥），不能随仓库分发。
- 安装 NSP 还需要签名补丁（sigpatch），并且比 `.nro` 更容易触发账号风险。因此本项目默认只发布 `.nro`。

如果确实需要桌面图标，推荐用社区的 NSP forwarder 工具，指向 `sd:/switch/netease-switch.nro`，不必重新打包程序本体。

## 构建

官方 devkitPro 镜像已经包含全部依赖：

```bash
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  bash -lc 'export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH; make -j$(nproc)'
```

本机安装 devkitPro 时需要的包：

```bash
pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-curl switch-mbedtls
```

产物为仓库根目录的 `netease-switch.nro`（内含 romfs 里的 CA 证书）。

## 实现说明

- **请求加密**：`source/crypto.cpp` 复刻网页端 `weapi` 方案（两次 AES-128-CBC + 1024 位 RSA，无填充、密钥反转），参数经 mbedTLS 计算。
- **网络**：`source/http.cpp` 基于 switch-curl，手动维护 Cookie；TLS 使用仓库内置的 Mozilla CA 包（`romfs/cacert.pem`）。
- **播放**：音频下载到内存后交给 `dr_mp3` / `dr_flac` 解码，经环形缓冲送入 SDL 音频设备；按文件采样率重开设备，不做重采样。
- **界面**：SDL2 + SDL2_ttf，字体直接取自系统共享字体（`pl` 服务），不打包任何字体文件。
- **二维码**：Nayuki QR Code generator。

## 已知限制

- 音质取决于账号权限和歌曲本身，未登录一般只能拿到较低码率；VIP/版权受限歌曲会提示无法播放。
- 没有封面、缓存、后台播放、下载和歌单编辑，后续版本再说。
- 网易云网页接口不是公开 API，改动后需要同步更新 `source/api.cpp`。
- 只在 Ryujinx/真机的基本流程上验证过，长时间播放稳定性尚未验证。

## 第三方组件

| 组件 | 许可 |
| --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [dr_libs](https://github.com/mackron/dr_libs) | Public Domain / MIT-0 |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) | MIT |
| [curl CA bundle](https://curl.se/ca/) | MPL 2.0 |

## 许可

本项目代码以 MIT 许可发布，见 [LICENSE](LICENSE)。
