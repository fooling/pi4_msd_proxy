# 外层 nginx 配置 — 在 pi4_msd_proxy 前面加一层反向代理

`pi4_msd_proxy` 本身（监听 8887）已经能直接给播放器用，**不需要 nginx**。
加一层 nginx 只在你想要下面这些时才有意义：

- 对外只暴露 80/443 一个端口，藏掉 8887 和上游 msd_lite 地址；
- 套 TLS（`https://`）/ 加 basic auth / 限制来源 IP；
- 一个域名下同时挂别的服务，靠 path 分流。

> ⚠️ **直播流必须关缓存。** 这是最容易踩的坑：nginx 默认 `proxy_buffering on`，
> 再加上 `proxy_cache`/`proxy_store` 就会把这条"永不结束的 TS 响应"当成普通文件存起来，
> 之后每次重连都从**缓存开始那一刻**喂起 —— 表现就是"每次进去都是同一个时间点、画面不随真实时间走"。
> 下面的配置已经把这些全部关掉。

---

## 方式 A：透传式（保留 msd_lite 风格 URL，推荐）

播放器 URL 里照样带上游地址，nginx 只做转发，不登记频道。

```nginx
# /etc/nginx/conf.d/iptv.conf
server {
    listen 80;
    server_name tv.lan;          # 换成你的域名/IP

    # http://tv.lan/tv/192.168.1.2:8888/rtp/239.0.0.1:1234
    #                └──────── 原 msd_lite 地址(去 http://) ────────┘
    location /tv/ {
        proxy_pass http://127.0.0.1:8887/;   # 末尾 / 把 /tv/ 前缀剥掉

        # —— 直播流必须的：全部关缓存/缓冲 ——
        proxy_cache              off;
        proxy_store              off;
        proxy_buffering          off;
        proxy_request_buffering  off;
        chunked_transfer_encoding off;

        # —— 长连接，别被超时掐断 ——
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }
}
```

改 m3u 时把前缀换成 `http://tv.lan/tv/`：

```bash
sed -E 's#https?://#http://tv.lan/tv/#g' old.m3u > new.m3u
# => http://tv.lan/tv/192.168.1.2:8888/rtp/239.0.0.1:1234
#    http://tv.lan/tv/192.168.1.2:8888/rtp/239.0.0.1:1234?target-resolution=720p
```

## 方式 B：固定频道映射（干净 path，藏掉上游地址）

每个频道写死一条 location，对外只露 `/sctv` 这种短路径。

```nginx
server {
    listen 80;
    server_name tv.lan;

    location = /sctv {
        proxy_pass http://127.0.0.1:8887/192.168.1.2:8888/rtp/239.0.0.1:1234;
        include /etc/nginx/snippets/live-noproxybuf.conf;   # 见下
    }
    location = /sctv-720p {
        proxy_pass http://127.0.0.1:8887/192.168.1.2:8888/rtp/239.0.0.1:1234?target-resolution=720p;
        include /etc/nginx/snippets/live-noproxybuf.conf;
    }
}
```

把那串"关缓存"指令抽成 snippet，避免每个 location 重复：

```nginx
# /etc/nginx/snippets/live-noproxybuf.conf
proxy_cache              off;
proxy_store              off;
proxy_buffering          off;
proxy_request_buffering  off;
chunked_transfer_encoding off;
proxy_http_version 1.1;
proxy_set_header Connection "";
proxy_read_timeout 3600s;
proxy_send_timeout 3600s;
```

m3u 直接写 `http://tv.lan/sctv` / `http://tv.lan/sctv-720p`。

---

## 重要细节

- **`http { }` 里若设过 `proxy_cache xxx;` 会被继承到所有 location**，即使 location 没写 cache
  也会生效；所以上面**显式 `proxy_cache off;`** 才能盖掉，别省。
- `proxy_pass` 末尾**带 `/`**（方式 A）才会剥掉 `location` 前缀；不带 `/` 会把 `/tv/...` 整段拼到上游。
- 想加 TLS：把 `listen 80;` 换成 `listen 443 ssl;` 配上 `ssl_certificate*`，其余不变。
- 验证没缓存：连上后看响应头不应有 `X-Cache`/`Age`，且 `curl -s http://tv.lan/sctv | head -c 0`
  每次重连画面都从"当前实时"开始，而不是固定某一刻。
- 改完一律 `sudo nginx -t && sudo systemctl reload nginx`。

## 到底要不要 nginx？

| 场景 | 建议 |
|------|------|
| 内网自己看 | 直接 `http://<pi>:8887/...`，**不用 nginx** |
| 要 TLS / 域名 / 藏上游 / basic auth | 上 nginx，按方式 A |
| 想给频道起短名 | 方式 B |

> nginx 这层纯属"门面"，不参与转码/透传逻辑；真正干活的还是 8887 上的 `pi4_msd_proxy`。
