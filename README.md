# pi4_msd_proxy — Pi4 IPTV 代理 (msd_lite 风格) + 按需硬件转码

一个单功能守护进程：像 msd_lite/udpxy 那样 **URL 里直接带上游地址**，零频道登记。
**默认纯透传**（原始编码/分辨率，全程不碰 ffmpeg）；只有客户端要求降码时才对那一路挂硬件转码。

- **默认 HTTP 真透传**：纯字节中继，原始质量，零 ffmpeg；连分块编码也原生解。4K HEVC 原样转发。
- **按需转码**：URL 加 `?target-resolution=720p` 才对该会话起 ffmpeg（drm 硬解 + h264_v4l2m2m 硬编）。
- **并发**：每个(源+档位)一路会话，多客户端 `shared_ptr` 零拷贝扇出；海量透传 + 个别转码并行。
- **快速起播**：precache 一连上就灌最近数据；缓冲按 msd_lite 4K 档。
- **纯 POSIX + C++ 标准库**，无第三方依赖；ffmpeg 仅在转码/非 http 源透传时作引擎。

## 透明代理模型
```
现有:  http://192.168.1.2:8888/rtp/239.0.0.1:1234
改成:  http://<pi>:8887/192.168.1.2:8888/rtp/239.0.0.1:1234
            └ pi 代理 ┘└──── 原 msd_lite 地址(去掉 http://) ────┘
```
数据流：
```
默认(透传):  播放器 ─> [pi4_msd_proxy 原生 HTTP 中继, 无 ffmpeg] ─> 上游 (原始 4K HEVC 原样)
降码(转码):  播放器 ?target-resolution=720p ─> [起 ffmpeg: drm硬解+h264硬编] ─> 上游
两者共享会话注册表, 可同时并发; 同(源+档位)只一路, 扇出给该会话全部客户端。
```

## 在 Pi4 上编译安装

```bash
sudo apt install -y build-essential        # 若无 g++
cd pi4_msd_proxy
make                                        # 单文件二进制
sudo make install                           # -> /usr/local/bin, 配置 -> /usr/local/etc/
sudo nano /usr/local/etc/pi4_msd_proxy.conf # 端口/默认档/缓冲
sudo systemctl daemon-reload
sudo systemctl enable --now pi4_msd_proxy
journalctl -u pi4_msd_proxy -f
```

## 启动命令 (msd_lite 风格)

```
pi4_msd_proxy [-c 配置文件] [-d] [-p pid文件] [-v] [-h]
  -c FILE  配置文件 (默认 /usr/local/etc/pi4_msd_proxy.conf)
  -d       后台运行 (daemon)
  -p FILE  写 pid 文件
```
手动后台：`pi4_msd_proxy -c /usr/local/etc/pi4_msd_proxy.conf -d -p /run/pi4_msd_proxy.pid`
（systemd 用前台模式，不加 `-d`。）

## 播放 / 改 m3u

把现有 m3u 里每条 URL 的 `http://` 前缀替换成 `http://<pi-ip>:8887/`：
```bash
sed -E 's#https?://#http://<pi-ip>:8887/#g' old.m3u > new.m3u
```
**默认纯透传(原始质量)。** 需要降码省带宽时，URL 末尾加查询参数（改分辨率自动配码率）：
```
http://<pi-ip>:8887/192.168.1.2:8888/rtp/239.0.0.1:1234?target-resolution=720p
```
档位自动码率：1080p→6000k，720p→2500k，540p→1800k，480p→1200k（也可 `&vb=3000k` 手动指定）。
打开 `http://<pi-ip>:8887/` 有用法说明页。

## 配置

见 `pi4_msd_proxy.conf.example`。全局：`listen` / `ffmpeg` / `idle_timeout`；默认档
`height`(0=透传) / `video_bitrate` / `audio` / `fps`；缓冲(msd 4K 档)
`ring_buf_size` / `precache` / `snd_buf` / `read_block`。改完 `sudo systemctl restart pi4_msd_proxy`。

## 外层 nginx（可选）

内网自己看直接连 `http://<pi>:8887/...` 即可，**不需要 nginx**。需要 TLS/域名/藏上游/短路径时，
在前面套一层反向代理见 [`NGINX.md`](NGINX.md)。⚠️ 直播流必须关 `proxy_cache`/`proxy_buffering`，
否则每次重连画面都冻在同一时刻（缓存把无限长的 TS 当文件存了）。

## 说明 / 限制

- **默认透传不碰 ffmpeg**，原始质量、零损失、最省 CPU；4K HEVC 原样发给能解 HEVC 的设备。
- 转码路径输出固定 H.264（Pi4 只有 H.264 硬编、上限 1080p；无 HEVC 硬编）。瓶颈是 4K 缩放，
  已用多线程 `fast_bilinear` 软缩绕过（720p 实测 ~23% CPU/1.06× 稳）。
- **Pi4 codec 块同时多路转码可能争用甚至卡死**：透传随便多路；转码先一路稳了再加，看 `journalctl`。
- HEVC 硬解走 `-hwaccel drm`（不是 `hevc_v4l2m2m`，Pi4 上不可用）——已内置正确配方。
