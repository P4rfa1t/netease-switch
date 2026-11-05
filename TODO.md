# 待实现功能备忘录

> 对照官方 TV 客户端（1.1.80）的反编译结果与实机截图整理。
> 反编译材料：`D:\REA\NMC-TV-Port`
> 状态：`未开始` / `进行中` / `已完成` / `不打算做`

## 一、界面（按截图还原）

| 功能 | 说明 | 状态 |
| --- | --- | --- |
| 顶栏双标签 | 只保留「推荐」「我的」，已完成 | 已完成 |
| 推荐页 | 改用 `music/multi/terminal/tv/homepage/get` 下发的 blocks，无数据时回退固定四卡 | 已完成 |
| 我的页 | 资料卡、会员横幅位、四宫格入口 | 已完成 |
| 歌单详情页 | 双列歌曲列表（每页 8 首）、播放全部/心动模式按钮、分页器 1/82 | 进行中 |
| 我的歌单页 | 创建/收藏分别走 `user/playlist` 与 `user/playlist/collect` | 已完成 |
| 收藏专辑 / 收藏视频 | `album/sublist` / `mlog/mycollect/bytime/get` | 阻塞：两者均返回 301 |
| 关注歌手页 | 圆形头像网格 + 艺名 | 阻塞：`user/follow/users/mixed/get/v2` 返回 400，`playlist/tv/detail` 返回 401 |
| 黑胶播放页 | 唱片、唱臂、进度条、控制行 | 已完成 |
| 歌词页 | 逐行滚动 + 翻译 + 罗马音 | 部分（翻译已有，罗马音未做） |
| 播放页控件图标 | 用 SDL 图形绘制三角/暂停条，避免字体缺字 | 已完成 |
| 搜索页 | TV 版用自绘键盘 + 热搜榜；Switch 端改用系统键盘 | 不打算做自绘键盘 |

## 二、播放能力

| 功能 | 说明 | 状态 |
| --- | --- | --- |
| MP3 / FLAC 解码 | dr_mp3 / dr_flac | 已完成 |
| M4A / AAC 解码 | libavformat + libavcodec，用于云盘原文件 | 已完成 |
| 红心 / 取消喜欢 | `/eapi/song/like` | 已完成 |
| 音质切换 | 128K / 192K / 320K / 无损，写入 quality.txt | 已完成 |
| 播放模式 | 顺序 / 单曲 / 随机（`-` 键） | 已完成 |
| 逐字歌词（YRC） | `klyric` 字段，`[行起始,行时长](字起始,字时长,0)字` | 未开始 |
| 罗马音歌词 | `romalrc` 字段 | 未开始 |
| 音效（EQ / 混响） | 素材已有 22 套 EQ + 10 套混响，需要 biquad + 早期反射实现 | 未开始 |
| 频谱可视化 | `1661889871148fft.txt` 已在素材里 | 未开始 |

## 三、跨端与同步

| 功能 | 说明 | 状态 |
| --- | --- | --- |
| 最近播放同步 | `/eapi/v1/play/record` 已能读；写侧未找到通用进度上报接口 | 部分 |
| 跨端续播 | 依赖 `homepage/tv/block/page` + `extInfo`，返回 `CrossContinueVO`（deviceName/progress/sessionId）；无独立上报端点 | 未开始（需逆向 extInfo） |
| 播放进度上报 | 反编译里只有播客的 `dj/playrecord/upload`，歌曲侧未找到 | 未开始 |

## 四、其他页面与能力

| 功能 | 说明 | 状态 |
| --- | --- | --- |
| 每日推荐完整页 | 当前只在推荐卡里取第一首 | 未开始 |
| 私人漫游 / FM | `zone/fm/get`、`v1/radio/get` | 未开始 |
| 心动模式 | `/api/playmode/intelligence/list` | 未开始 |
| 歌单广场 / 分类 | `playlist/tv/category/get`、`playlist/tv/category/list` | 未开始 |
| 专辑详情 | `/eapi/v1/album/{id}` | 未开始 |
| 歌手详情 | `/eapi/v1/artist/{id}` | 未开始 |
| 搜索多类型 | 歌手 / 专辑 / 歌单（当前只有单曲） | 未开始 |
| 评论查看 | `/eapi/v1/resource/comments/R_SO_4_{id}` | 未开始 |
| 云盘列表页 | `v1/cloud/get` 在 eapi 下返回 301；云盘歌已可从歌单播放 | 未开始 |
| 播客 / 电台 | `dj/program/list`、`v6/dj/program/byradio` | 不打算做 |
| MV / 视频播放 | 需要视频解码与渲染管线 | 不打算做 |
| 氛围漫游 / 随心唱 / 亲子 | TV 专有模块，用户明确不要 | 不打算做 |

## 五、已知问题

| 问题 | 状态 |
| --- | --- |
| 手机扫码页显示设备名 | 已改用 `/st/platform/scanlogin` 并带 `hdw_model=NintendoSwitch`，待确认 |
| 云盘歌取流格式 | 服务端对设备返回原文件（M4A）、对脚本返回转码 MP3，原因未明；已用 ffmpeg 兜底 |
| NRO 体积 23.8 MB | ffmpeg 静态链接带的编解码器过多，可考虑精简编译 |
| 真机播放稳定性 | 长时间播放未验证 |
