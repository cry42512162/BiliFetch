#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bili_bridge.py —— bili-digest 的 JSON 服务桥（C++/Qt 子进程集成用）

协议（stdin/stdout，每行一个 JSON，UTF-8）：
  请求:  {"id": 1, "cmd": "<command>", "params": {...}}
  响应:  {"id": 1, "ok": true,  "data": {...}}
         {"id": 1, "ok": false, "error": "..."}
  进度:  {"id": 1, "event": "progress", "stage": "...", "detail": "..."}  （异步穿插）

命令一览：
  ping      连通性自检
  status    B站凭据状态（复用 bili_digest.load_saved_cookie）
  classify  输入类型识别（video/dynamic/article/up/keyword），不联网
  search    关键词搜视频（search_bilibili_videos）
  digest    视频完整提取：元信息 + 官方AI总结 + 字幕 + 评论树（结构化）

设计原则：
  - 不修改 bili_digest.py 本体，仅 import 其内部函数（与 bili-batch 同一做法）
  - 业务失败以 ok=false + error 返回，进程不退出；仅协议级错误终止
"""

import base64
import io
import json
import re
import sys
import time
import traceback
import urllib.parse
import urllib.request
from pathlib import Path

# ---------------------------------------------------------------------------
# 环境准备
# ---------------------------------------------------------------------------

# stdout/stdin 强制 UTF-8（Windows 控制台默认 GBK 会炸）
for _s in (sys.stdout, sys.stdin):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

import os

BILI_DIGEST_DIR = os.environ.get(
    "BILI_DIGEST_DIR", r"D:\软件\AI工具\依赖\bili-digest"
)
if BILI_DIGEST_DIR not in sys.path:
    sys.path.insert(0, BILI_DIGEST_DIR)

import bili_digest as bd  # noqa: E402  复用插件全部内部函数


# ---------------------------------------------------------------------------
# 输出工具
# ---------------------------------------------------------------------------

def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _progress(req_id, stage: str, detail: str = "") -> None:
    _emit({"id": req_id, "event": "progress", "stage": stage, "detail": detail})


# ---------------------------------------------------------------------------
# 命令实现
# ---------------------------------------------------------------------------

def cmd_ping(_req_id, _params) -> dict:
    return {
        "pong": True,
        "python": sys.version.split()[0],
        "bili_digest_dir": BILI_DIGEST_DIR,
        "ts": int(time.time()),
    }


def cmd_status(_req_id, _params) -> dict:
    cookie = bd.load_saved_cookie()
    has = bool(cookie and "SESSDATA" in cookie)
    return {
        "has_cookie": has,
        "cred_path": str(
            os.path.join(os.path.expanduser("~"), ".dsh", "bilibili_credentials.json")
        ),
        "output_dir": str(bd.ensure_default_dir()),
    }


def cmd_classify(_req_id, params) -> dict:
    target = str(params.get("target", "")).strip()
    if not target:
        raise ValueError("target 不能为空")
    return {"target": target, "type": bd.classify_bili_target(target)}


def cmd_search(req_id, params) -> dict:
    keyword = str(params.get("keyword", "")).strip()
    if not keyword:
        raise ValueError("keyword 不能为空")
    count = int(params.get("count", 5))
    order = str(params.get("order", "totalrank"))
    _progress(req_id, "search", f"搜索关键词: {keyword}")
    videos = bd.search_bilibili_videos(keyword, count=count, order=order)
    return {"keyword": keyword, "count": len(videos), "videos": videos}


def cmd_digest(req_id, params) -> dict:
    """视频完整提取 —— 与 run_digest 同流程，但返回结构化 JSON（含完整评论树）"""
    target = str(params.get("target", "")).strip()
    if not target:
        raise ValueError("target 不能为空")

    comment_mode = str(params.get("comment_mode", "hot_60"))
    max_comments = int(params.get("max_comments", 20))
    include_replies = bool(params.get("include_replies", True))
    include_subtitles = bool(params.get("include_subtitles", True))
    include_ai = bool(params.get("include_ai", True))
    include_comments = bool(params.get("include_comments", True))
    save_markdown = bool(params.get("save_markdown", True))

    cookie = bd.load_saved_cookie()

    _progress(req_id, "resolve", f"解析目标: {target}")
    bvid = bd.resolve_target_bvid(target)
    if not bvid.startswith("BV"):
        raise ValueError(f"无法从输入中解析出有效的 BV 号: {target}")

    _progress(req_id, "video_info", f"获取视频信息: {bvid}")
    v_info = bd.get_video_view(bvid, cookie=cookie)
    aid = v_info["aid"]
    cid = v_info["cid"]
    owner = v_info.get("owner", {}) or {}
    up_mid = owner.get("mid", 0)
    up_name = owner.get("name", "未知UP主")
    pubdate = v_info.get("pubdate", 0)
    title = v_info.get("title", bvid)
    stat = v_info.get("stat", {}) or {}
    total_reply = stat.get("reply", 0)

    ai_info = {}
    if include_ai:
        _progress(req_id, "ai_conclusion", "获取官方 AI 总结")
        ai_info = bd.get_ai_conclusion(bvid, cid, up_mid, cookie=cookie)

    sub_info = {}
    if include_subtitles:
        _progress(req_id, "subtitles", "获取字幕")
        sub_info = bd.get_subtitles_detail(bvid, aid, cid, cookie=cookie)

    comments = []
    mode_desc = ""
    if include_comments:
        _progress(req_id, "comments", f"抓取评论（mode={comment_mode}, 上限 {max_comments}）")
        comments, mode_desc = bd.get_video_comments(
            aid=aid,
            total_reply_count=total_reply,
            comment_mode=comment_mode,
            max_count=max_comments,
            include_replies=include_replies,
            cookie=cookie,
        )

    # 可选落盘 Markdown（与插件产物一致，便于归档/复用）
    md_file = None
    if save_markdown:
        _progress(req_id, "save_markdown", "保存 Markdown 归档")
        md_content = bd.render_markdown(
            v_info, ai_info, sub_info, comments,
            include_subtitles=include_subtitles, comment_mode_desc=mode_desc,
        )
        filename = bd.build_video_filename(pubdate, up_name, title, bvid)
        out_file = _out_dir(params) / filename
        out_file.parent.mkdir(parents=True, exist_ok=True)
        out_file.write_text(md_content, encoding="utf-8")
        md_file = str(out_file.resolve())

    # AI 总结结构化（官方接口返回 model_result 列表）
    ai_summary = []
    model_result = (ai_info or {}).get("model_result") or []
    for seg in model_result:
        if not isinstance(seg, dict):
            continue
        ai_summary.append({
            "title": seg.get("title", ""),
            "summary": seg.get("summary", ""),
            "timestamp_from": seg.get("timestamp_from", 0),
            "timestamp_to": seg.get("timestamp_to", 0),
        })

    return {
        "kind": "video",
        "video": {
            "bvid": bvid,
            "aid": aid,
            "cid": cid,
            "title": title,
            "up_name": up_name,
            "up_mid": up_mid,
            "pubdate": pubdate,
            "pubdate_text": bd.format_pubdate(pubdate) if pubdate else "",
            "desc": v_info.get("desc", ""),
            "cover": v_info.get("pic", ""),
            "duration": v_info.get("duration", 0),
            "url": f"https://www.bilibili.com/video/{bvid}",
            "stat": {
                "view": stat.get("view", 0),
                "danmaku": stat.get("danmaku", 0),
                "reply": stat.get("reply", 0),
                "like": stat.get("like", 0),
                "coin": stat.get("coin", 0),
                "favorite": stat.get("favorite", 0),
                "share": stat.get("share", 0),
            },
        },
        "ai": {
            "has": bool(ai_summary),
            "segments": ai_summary,
            "raw": ai_info,
        },
        "subtitles": {
            "has": bool((sub_info or {}).get("has_subtitles")),
            "lan_doc": (sub_info or {}).get("lan_doc", ""),
            "is_ai": bool((sub_info or {}).get("is_ai")),
            "lines": (sub_info or {}).get("lines", []),
        },
        "comments": {
            "mode": comment_mode,
            "mode_desc": mode_desc,
            "count": len(comments),
            "items": comments,
        },
        "markdown_file": md_file,
    }


# ---------------------------------------------------------------------------
# auto 模糊路由：classify -> 分发到五类管线（结构化返回）
# ---------------------------------------------------------------------------

def _fetch_comments_for(req_id, aid, total_reply, params, cookie, refer_url="",
                        comment_type=1):
    """评论抓取统一入口（视频/专栏/动态共用 get_video_comments）"""
    comment_mode = str(params.get("comment_mode", "hot_60"))
    max_comments = int(params.get("max_comments", 20))
    include_replies = bool(params.get("include_replies", True))
    if not bool(params.get("include_comments", True)):
        return [], "", comment_mode
    _progress(req_id, "comments",
              f"抓取评论（mode={comment_mode}, 上限 {max_comments}）")
    comments, mode_desc = bd.get_video_comments(
        aid=aid, total_reply_count=total_reply, comment_mode=comment_mode,
        max_count=max_comments, include_replies=include_replies,
        cookie=cookie, comment_type=comment_type, refer_url=refer_url,
    )
    return comments, mode_desc, comment_mode


def _save_md(req_id, md_content, out_file):
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(md_content, encoding="utf-8")
    return str(out_file.resolve())


def _out_dir(params) -> Path:
    """输出目录：params.output_dir 优先，否则插件默认目录"""
    p = str(params.get("output_dir") or "").strip()
    if p:
        path = Path(p)
        path.mkdir(parents=True, exist_ok=True)
        return path
    return bd.ensure_default_dir()


def _comments_block(comments, mode_desc, comment_mode):
    return {
        "mode": comment_mode,
        "mode_desc": mode_desc,
        "count": len(comments),
        "items": comments,
    }


def _fetch_article(req_id, target, params, cookie):
    """专栏结构化提取（对齐 run_article_digest 流程，返回结构化评论）"""
    _progress(req_id, "resolve", f"解析专栏: {target}")
    cv_id = bd.resolve_article_id(target)
    _progress(req_id, "article", f"获取专栏详情 cv{cv_id}")
    try:
        detail = bd.get_article_detail(cv_id, cookie=cookie)
    except RuntimeError as e:
        msg = str(e)
        if "INITIAL_STATE" in msg or "412" in msg or "验证码" in msg:
            # 网页端触发安全验证（未登录 + 高频访问 www.bilibili.com/read/）
            raise RuntimeError(
                msg + "｜原因：B站网页返回了验证码/风控页。"
                      "请菜单「账号 → 登录」扫码后重试，或等待几分钟冷却再抓；"
                      "视频/动态/清单走 API 接口不受此限制。") from e
        raise
    parsed = bd.parse_article(detail)
    parsed["cv_id"] = str(cv_id)
    parsed["url"] = f"https://www.bilibili.com/read/cv{cv_id}"

    total_reply = parsed.get("stat", {}).get("comment", 0)
    comments, mode_desc, comment_mode = _fetch_comments_for(
        req_id, parsed.get("comment_id"), total_reply, params, cookie,
        refer_url=parsed["url"], comment_type=1)

    md_file = None
    if bool(params.get("save_markdown", True)):
        _progress(req_id, "save_markdown", "保存 Markdown 归档")
        md = bd.render_article_markdown(parsed, comments, mode_desc)
        md_file = _save_md(req_id, md,
                           _out_dir(params) / bd.build_article_filename(parsed))

    author = parsed.get("author", {}) or {}
    return {
        "kind": "article",
        "detected_type": "ARTICLE",
        "article": {
            "cv_id": parsed["cv_id"],
            "title": parsed.get("title", ""),
            "author": author.get("name", ""),
            "pubdate": author.get("pub_date", 0),
            "pubdate_text": bd.format_pubdate(author.get("pub_date", 0) or 0),
            "url": parsed["url"],
            "body_md": parsed.get("body_md", ""),
            "stat": parsed.get("stat", {}),
        },
        "comments": _comments_block(comments, mode_desc, comment_mode),
        "markdown_file": md_file,
    }


def _fetch_dynamic(req_id, target, params, cookie):
    """单条动态结构化提取（对齐 run_dynamic_digest 流程）"""
    _progress(req_id, "resolve", f"解析动态: {target}")
    dyn_id = bd.resolve_dynamic_id(target)
    _progress(req_id, "dynamic", f"获取动态详情 {dyn_id}")
    item = bd.get_dynamic_detail(dyn_id, cookie=cookie)
    parsed = bd.parse_dynamic_item(item)

    total_reply = parsed.get("stat", {}).get("comment", 0)
    comments, mode_desc, comment_mode = _fetch_comments_for(
        req_id, parsed.get("comment_id"), total_reply, params, cookie,
        refer_url=f"https://t.bilibili.com/{dyn_id}",
        comment_type=int(parsed.get("comment_type", 1) or 1))

    md_file = None
    if bool(params.get("save_markdown", True)):
        _progress(req_id, "save_markdown", "保存 Markdown 归档")
        md = bd.render_dynamic_markdown(parsed, comments, comment_mode_desc=mode_desc)
        md_file = _save_md(req_id, md,
                           _out_dir(params) / bd.build_dynamic_filename(parsed))

    author = parsed.get("author", {}) or {}
    stat = parsed.get("stat", {}) or {}
    return {
        "kind": "dynamic",
        "detected_type": "DYNAMIC",
        "dynamic": {
            "dynamic_id": parsed.get("dynamic_id", dyn_id),
            "dyn_type": parsed.get("dyn_type", ""),
            "author": author.get("name", ""),
            "pubdate": author.get("pub_date", 0),
            "pubdate_text": bd.format_pubdate(author.get("pub_date", 0) or 0),
            "title": parsed.get("content_title")
                     or (parsed.get("desc_text") or "")[:60],
            "text": parsed.get("desc_text", ""),
            "images": parsed.get("content_images", []) or [],
            "url": f"https://t.bilibili.com/{dyn_id}",
            "stat": {"comment": stat.get("comment", 0), "like": stat.get("like", 0),
                     "forward": stat.get("forward", 0)},
        },
        "comments": _comments_block(comments, mode_desc, comment_mode),
        "markdown_file": md_file,
    }


def _get_user_videos(mid, cookie, page_size=50, max_pages=200, page_retries=6):
    """
    分页拉取 UP 主全部投稿视频（x/space/wbi/arc/search，WBI 签名）。
    与插件 get_user_video_list 同接口，但**逐页长退避重试**：实测 -352 风控
    偶发命中（约 1/3 请求），3 次内短间隔重试仍可能失败；这里 6 次重试、
    指数间隔 3s→18s，且每次重建 SPI buvid 成对指纹。
    返回 (videos, total)。
    """
    mixin_key = bd.get_wbi_mixin_key(cookie)
    all_videos = []
    total = None
    vlist = []
    for pn in range(1, max_pages + 1):
        last_err = None
        for attempt in range(page_retries):
            try:
                # 每次尝试重建 headers（SPI 换新 buvid 成对指纹）
                headers = bd.build_dynamic_headers(mid, cookie=cookie)
                params = {"mid": str(mid), "ps": str(page_size), "pn": str(pn),
                          "order": "pubdate", "platform": "web",
                          "web_location": "1550101"}
                signed = bd.enc_wbi(params, mixin_key)
                url = (f"https://api.bilibili.com/x/space/wbi/arc/search"
                       f"?{urllib.parse.urlencode(signed)}")
                data = bd.http_get_json(url, headers=headers, cookie=cookie)
                code = data.get("code")
                if code != 0:
                    last_err = f"[{code}] {data.get('message')}"
                    time.sleep(3 * (attempt + 1))   # 3s/6s/9s… 递增退避
                    continue
                d = data.get("data") or {}
                if total is None:
                    total = int((d.get("page") or {}).get("count") or 0)
                vlist = (d.get("list") or {}).get("vlist") or []
                for v in vlist:
                    all_videos.append({
                        "bvid": v.get("bvid", ""),
                        "aid": v.get("aid", 0),
                        "title": v.get("title", ""),
                        "pubdate": int(v.get("created") or 0),
                        "duration": v.get("length", ""),
                        "play": v.get("play", 0),
                        "description": (v.get("description") or "").strip()[:120],
                    })
                last_err = None
                break
            except Exception as e:
                last_err = str(e)
                time.sleep(3 * (attempt + 1))
        if last_err is not None:
            # 连续失败：已有部分结果则返回部分，否则抛错
            if all_videos:
                break
            raise RuntimeError(f"获取 UP 主投稿视频列表失败: {last_err}")
        if total is not None and len(all_videos) >= total:
            break
        if len(vlist) < page_size:
            break
        time.sleep(1.2)
    return all_videos, (total if total is not None else len(all_videos))


def _get_user_articles(mid, cookie, max_pages=200):
    """
    分页拉取 UP 主全部专栏（x/space/article，实测可用：按发布时间倒序，
    data.articles / data.count 分页）。返回 (articles, total)。
    """
    all_articles = []
    total = None
    headers = bd.build_dynamic_headers(mid, cookie=cookie)
    for pn in range(1, max_pages + 1):
        url = (f"https://api.bilibili.com/x/space/article"
               f"?mid={mid}&ps=30&pn={pn}")
        data = bd.http_get_json(url, headers=headers, cookie=cookie)
        if data.get("code") != 0:
            if all_articles:
                break
            raise RuntimeError(
                f"获取 UP 主专栏列表失败: [{data.get('code')}] {data.get('message')}")
        d = data.get("data") or {}
        if total is None:
            total = int(d.get("count") or 0)
        arts = d.get("articles") or []
        if not arts:
            break
        for a in arts:
            stat = a.get("stats") or {}
            author = a.get("author") or {}
            all_articles.append({
                "cv_id": str(a.get("id", "")),
                "title": (a.get("title") or "").strip(),
                "pubdate": int(a.get("publish_time") or a.get("ctime") or 0),
                "words": a.get("words", 0),
                "summary": (a.get("summary") or "").strip()[:120],
                "view": stat.get("view", 0),
                "like": stat.get("like", 0),
                "reply": stat.get("reply", 0),
                "author": author.get("name", ""),
                "url": f"https://www.bilibili.com/read/cv{a.get('id', '')}",
            })
        if total is not None and len(all_articles) >= total:
            break
        if len(arts) < 30:
            break
        time.sleep(0.5)
    return all_articles, (total if total is not None else len(all_articles))


def _fmt_num(n) -> str:
    """数字千分化：12345 -> 1.2万"""
    try:
        n = int(n or 0)
    except (TypeError, ValueError):
        return str(n or 0)
    if n >= 100000000:
        return f"{n / 100000000:.1f}亿"
    if n >= 10000:
        return f"{n / 10000:.1f}万"
    return str(n)


def _safe_name(s: str, limit: int = 40) -> str:
    """文件名安全化"""
    out = "".join("_" if c in '\\/:*?"<>|\r\n\t' else c for c in str(s)).strip()
    return out[:limit] or "未命名"


def _save_list_md(req_id, params, filename, md_content) -> str:
    """清单类归档：写入 output_dir（或插件默认目录），返回绝对路径"""
    out_file = _out_dir(params) / filename
    _progress(req_id, "save_markdown", "保存清单归档")
    return _save_md(req_id, md_content, out_file)


def _fetch_user(req_id, target, params, cookie):
    """
    UP 主主页 → 全部投稿视频 + 全部专栏（分两部分、按发布时间倒序）。
    只列清单不抓内容，由用户在 GUI 勾选后经批量队列抓取。
    """
    _progress(req_id, "resolve", f"解析 UP 主: {target}")
    mid = bd.resolve_up_mid(target)

    up_name = str(mid)
    # UP 名：card 接口稳定（acc/info 对无 dm 参数请求返回 -799）
    try:
        card = bd.http_get_json(
            f"https://api.bilibili.com/x/web-interface/card?mid={mid}",
            headers=bd.build_dynamic_headers(mid, cookie=cookie), cookie=cookie)
        if card.get("code") == 0:
            up_name = (((card.get("data") or {}).get("card")) or {}).get("name") or up_name
    except Exception:
        pass

    _progress(req_id, "up_videos", f"拉取 {up_name} 的全部投稿视频清单")
    videos, v_total = _get_user_videos(mid, cookie)
    videos.sort(key=lambda v: v.get("pubdate") or 0, reverse=True)

    _progress(req_id, "up_articles", f"拉取 {up_name} 的全部专栏清单")
    try:
        articles, a_total = _get_user_articles(mid, cookie)
        articles.sort(key=lambda a: a.get("pubdate") or 0, reverse=True)
    except Exception as e:
        # 专栏清单失败不拖垮整个结果（如风控），置空并标注
        articles, a_total = [], 0
        _progress(req_id, "up_articles_warn", f"专栏清单获取失败: {e}")

    # ---- 归档：生成清单 Markdown（用户指定输出目录后必须有产物）----
    md_file = None
    if bool(params.get("save_markdown", True)):
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        lines = [
            f"# {up_name} · B站投稿清单",
            "",
            f"- 主页：https://space.bilibili.com/{mid}",
            f"- 生成时间：{now}",
            f"- 视频 **{v_total}** 个（本清单 {len(videos)} 个） · "
            f"专栏 **{a_total}** 篇（本清单 {len(articles)} 篇）",
            "- 按发布时间倒序；点击标题可跳转",
            "",
            f"## 🎬 视频（{len(videos)}）",
            "",
            "| # | 发布时间 | 标题 | 时长 | 播放 | BV号 |",
            "|---|---|---|---|---|---|",
        ]
        for i, v in enumerate(videos, 1):
            title = (v.get("title") or "").replace("|", "｜")
            lines.append(
                f"| {i} | {bd.format_pubdate(v.get('pubdate') or 0)} "
                f"| [{title}](https://www.bilibili.com/video/{v.get('bvid', '')}) "
                f"| {v.get('duration', '')} | {_fmt_num(v.get('play'))} "
                f"| {v.get('bvid', '')} |")
        lines += [
            "",
            f"## 📖 专栏（{len(articles)}）",
            "",
            "| # | 发布时间 | 标题 | 字数 | 阅读 | 链接 |",
            "|---|---|---|---|---|---|",
        ]
        for i, a in enumerate(articles, 1):
            title = (a.get("title") or "").replace("|", "｜")
            lines.append(
                f"| {i} | {bd.format_pubdate(a.get('pubdate') or 0)} "
                f"| [{title}]({a.get('url', '')}) "
                f"| {_fmt_num(a.get('words'))} | {_fmt_num(a.get('view'))} "
                f"| cv{a.get('cv_id', '')} |")
        lines += [
            "",
            "---",
            "",
            "在 BiliFetch「📂 UP主内容」页勾选条目 →「抓取选中」可批量抓取评论并归档。",
        ]
        fname = f"{time.strftime('%Y-%m-%d')}_{_safe_name(up_name)}_投稿清单.md"
        md_file = _save_list_md(req_id, params, fname, "\n".join(lines) + "\n")

    return {
        "kind": "up_content",
        "detected_type": "USER",
        "user": {"mid": str(mid), "name": up_name,
                 "url": f"https://space.bilibili.com/{mid}"},
        "video_total": v_total,
        "article_total": a_total,
        "videos": videos,
        "articles": articles,
        "markdown_file": md_file,
    }


def _fetch_keyword(req_id, target, params, cookie):
    """关键词：返回视频搜索结果清单（逐条可再 auto 抓取）"""
    count = max(1, min(20, int(params.get("count", 8)) or 8))
    order = str(params.get("order", "totalrank"))
    _progress(req_id, "search", f"搜索关键词: {target}")
    videos = bd.search_bilibili_videos(target, count=count, order=order, cookie=cookie)
    if not videos:
        raise RuntimeError(f"未找到关于「{target}」的相关视频")
    # ---- 归档：搜索结果清单 Markdown ----
    md_file = None
    if bool(params.get("save_markdown", True)):
        lines = [
            f"# 关键词搜索：{target}",
            "",
            f"- 生成时间：{time.strftime('%Y-%m-%d %H:%M:%S')}",
            f"- 排序：{order} · 共 {len(videos)} 条结果",
            "",
            "| # | 发布时间 | 标题 | UP主 | 播放 | BV号 |",
            "|---|---|---|---|---|---|",
        ]
        for i, v in enumerate(videos, 1):
            title = (v.get("title") or "").replace("|", "｜")
            author = (v.get("author") or "").replace("|", "｜")
            bvid = v.get("bvid", "")
            lines.append(
                f"| {i} | {bd.format_pubdate(v.get('pubdate') or 0)} "
                f"| [{title}](https://www.bilibili.com/video/{bvid}) "
                f"| {author} | {_fmt_num(v.get('play'))} | {bvid} |")
        lines += ["", "---", "",
                  "在 BiliFetch 中点「→ 批量」可把这些 BV 加入队列逐个抓取评论。"]
        fname = (f"{time.strftime('%Y-%m-%d')}_搜索_{_safe_name(target)}"
                 f"_结果清单.md")
        md_file = _save_list_md(req_id, params, fname, "\n".join(lines) + "\n")

    return {
        "kind": "keyword",
        "detected_type": "KEYWORD",
        "keyword": target,
        "videos": videos,
        "markdown_file": md_file,
    }


def cmd_auto(req_id, params) -> dict:
    """模糊路由主入口：任意输入 -> 自动识别五类并分发"""
    target = str(params.get("target", "")).strip()
    if not target:
        raise ValueError("target 不能为空")

    # 简写专栏号规范化：插件 classify 不认 "read/cv123" 简写（会误判 KEYWORD
    # 走成关键词搜索），统一升级为完整 URL（classify 对其返回 ARTICLE）
    m = re.fullmatch(r"(?i)read/cv(\d+)", target)
    if m:
        target = f"https://www.bilibili.com/read/cv{m.group(1)}"
        params = dict(params)
        params["target"] = target
        _progress(req_id, "normalize", f"专栏简写规范化: {target}")

    detected = bd.classify_bili_target(target)
    labels = {"VIDEO": "🎬 B站视频", "ARTICLE": "📖 B站专栏",
              "DYNAMIC": "💭 B站单条动态", "USER": "👤 UP主空间",
              "KEYWORD": "🔍 关键词检索"}
    _progress(req_id, "route", f"识别结果：{labels.get(detected, detected)}")

    cookie = bd.load_saved_cookie()

    if detected == "VIDEO":
        data = cmd_digest(req_id, params)
        data["detected_type"] = "VIDEO"
        return data
    if detected == "ARTICLE":
        return _fetch_article(req_id, target, params, cookie)
    if detected == "DYNAMIC":
        return _fetch_dynamic(req_id, target, params, cookie)
    if detected == "USER":
        return _fetch_user(req_id, target, params, cookie)
    return _fetch_keyword(req_id, target, params, cookie)


# ---------------------------------------------------------------------------
# batch 批量队列：多目标串行抓取 + 节流 + 412 退避（复用 bili-batch 逻辑）
# ---------------------------------------------------------------------------

BACKOFF_SECONDS = 420   # 412 风控退避，与 bili-batch 保持一致


def _is_risk_error(msg: str) -> bool:
    m = str(msg)
    return any(k in m for k in ("412", "Precondition", "限时", "风控", "-352"))


def _batch_one(req_id, target, params, cookie, attempt, backoff, sleep_s):
    """抓取单个目标；评论为空但实际总量>5 时按 bili-batch 规则退避重试"""
    p = dict(params)
    p["save_markdown"] = True
    data = cmd_auto(req_id, {**p, "target": target})

    if data.get("kind") == "video":
        total = (data.get("video", {}).get("stat", {}) or {}).get("reply", 0)
        got = (data.get("comments", {}) or {}).get("count", 0)
        if got == 0 and total > 5:
            raise RuntimeError(f"评论抓到 0 条（实际 {total}），需退避重试")
    return data


def cmd_batch(req_id, params) -> dict:
    """
    批量串行抓取。
    params:
      targets      list[str]  必填，BV/链接/任意可路由输入
      comment_mode / max_comments / count / include_*  同 auto
      sleep        int  两次抓取间基础休眠秒（默认 30）
      backoff      int  412 风控退避秒（默认 420，同 bili-batch）
      retries      int  单目标最大尝试次数（默认 3）
    """
    targets = params.get("targets") or []
    if isinstance(targets, str):
        targets = [t for t in targets.replace("\n", ",").split(",") if t.strip()]
    targets = [str(t).strip() for t in targets if str(t).strip()]
    if not targets:
        raise ValueError("targets 不能为空")

    sleep_s = max(0, int(params.get("sleep", 30)))
    backoff = max(1, int(params.get("backoff", BACKOFF_SECONDS)))
    retries = max(1, min(5, int(params.get("retries", 3))))
    cookie = bd.load_saved_cookie()

    n = len(targets)
    _progress(req_id, "batch_start", f"批量队列启动：{n} 个目标，"
              f"间隔 {sleep_s}s，风控退避 {backoff}s")

    results = []
    ok_count = 0
    for i, target in enumerate(targets, 1):
        _progress(req_id, "batch", f"[{i}/{n}] 抓取 {target}")
        item = {"target": target, "ok": False, "error": ""}
        last_err = ""
        for attempt in range(retries):
            try:
                data = _batch_one(req_id, target, params, cookie,
                                  attempt, backoff, sleep_s)
                item = {
                    "target": target, "ok": True, "kind": data.get("kind"),
                    "title": (data.get("video", {}).get("title")
                              or data.get("article", {}).get("title")
                              or data.get("dynamic", {}).get("title")
                              or target),
                    "comment_count": (data.get("comments", {}) or {}).get("count", 0),
                    "markdown_file": data.get("markdown_file"),
                    "error": "",
                }
                ok_count += 1
                break
            except Exception as e:
                last_err = f"{type(e).__name__}: {e}"
                if attempt < retries - 1:
                    wait = backoff if _is_risk_error(last_err) else sleep_s
                    _progress(req_id, "batch_retry",
                              f"[{i}/{n}] 失败: {last_err[:80]}；{wait}s 后重试")
                    time.sleep(wait)
        if not item["ok"]:
            item["error"] = last_err
        results.append(item)
        _progress(req_id, "batch_item",
                  f"[{i}/{n}] {'✅' if item['ok'] else '❌'} {item.get('title', target)[:50]}")
        if i < n and sleep_s > 0:
            time.sleep(sleep_s)

    fail = [r for r in results if not r["ok"]]
    _progress(req_id, "batch_done",
              f"批量完成：成功 {ok_count} / 失败 {len(fail)}")
    return {
        "kind": "batch",
        "total": n,
        "ok": ok_count,
        "failed": len(fail),
        "items": results,
        "sleep": sleep_s,
        "backoff": backoff,
    }


# ---------------------------------------------------------------------------
# 扫码登录（方案参考 bilibiliDownloader：passport web qrcode generate/poll）
# ---------------------------------------------------------------------------

_PASSPORT = "https://passport.bilibili.com"
_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
       "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")


def _passport_get(url):
    """passport 接口 GET，返回 (json_body, set_cookies_list)；需要响应头取 Cookie"""
    req = urllib.request.Request(url, headers={
        "User-Agent": _UA,
        "Referer": "https://www.bilibili.com/",
        "Accept": "application/json, text/plain, */*",
    })
    with urllib.request.urlopen(req, timeout=12) as resp:
        body = json.loads(resp.read().decode("utf-8", errors="ignore"))
        try:
            cookies = resp.headers.get_all("Set-Cookie") or []
        except Exception:
            cookies = []
    return body, cookies


def _assemble_cookie(set_cookies):
    """从 Set-Cookie 响应头集合提取登录关键 Cookie（SESSDATA/bili_jct/…）"""
    pairs = {}
    for item in set_cookies:
        first = item.split(";", 1)[0].strip()
        if "=" in first:
            k, v = first.split("=", 1)
            pairs[k.strip()] = v
    # 关键登录字段缺失则视为无效
    if "SESSDATA" not in pairs:
        return ""
    order = ["SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5",
             "sid", "SESSDATA_id"]
    parts = [f"{k}={pairs[k]}" for k in order if k in pairs]
    for k, v in pairs.items():          # 其余一并带上（如 bili_ticket 等）
        if k not in order and k.lower() not in ("expires", "path", "domain",
                                                "max-age", "samesite", "secure"):
            parts.append(f"{k}={v}")
    return "; ".join(parts)


def cmd_qr_start(_req_id, _params) -> dict:
    """拉取登录二维码：返回 qr_key / 扫码 URL / PNG base64"""
    body, _ = _passport_get(f"{_PASSPORT}/x/passport-login/web/qrcode/generate")
    if body.get("code") != 0:
        raise RuntimeError(f"获取二维码失败: [{body.get('code')}] {body.get('message')}")
    data = body.get("data") or {}
    url = data.get("url", "")
    key = data.get("qrcode_key", "")
    if not url or not key:
        raise RuntimeError("获取二维码失败：响应缺少 url/qrcode_key")

    png_b64 = ""
    try:
        import qrcode  # 可选依赖：缺失时仅返回 URL
        img = qrcode.make(url)
        buf = io.BytesIO()
        img.save(buf, "PNG")
        png_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    except Exception as e:
        _progress(_req_id, "qr_png_warn", f"二维码图片生成失败(可复制链接): {e}")
    return {"qr_key": key, "url": url, "png_base64": png_b64}


_QR_STATUS = {
    0: "ok",        # 登录成功（cookie 在 Set-Cookie 头）
    86101: "waiting",     # 未扫码
    86092: "scanned",     # 已扫码，待确认
    86038: "expired",     # 二维码过期
}


def cmd_qr_poll(_req_id, params) -> dict:
    """轮询扫码状态。status: waiting/scanned/ok/expired/error"""
    key = str(params.get("qr_key") or "").strip()
    if not key:
        raise ValueError("qr_key 不能为空")
    body, set_cookies = _passport_get(
        f"{_PASSPORT}/x/passport-login/web/qrcode/poll?qrcode_key="
        + urllib.parse.quote(key))
    if body.get("code") != 0:
        raise RuntimeError(f"轮询失败: [{body.get('code')}] {body.get('message')}")
    data = body.get("data") or {}
    inner = int(data.get("code", -1))
    status = _QR_STATUS.get(inner, "error")
    result = {
        "status": status,
        "code": inner,
        "message": data.get("message", ""),
        "refresh_token": data.get("refresh_token", ""),
    }
    if status == "ok":
        cookie = _assemble_cookie(set_cookies)
        if not cookie:
            raise RuntimeError("登录成功但未从响应头取到 SESSDATA，请改用手动粘贴 Cookie")
        result["cookie"] = cookie
    return result


def _nav_identity(cookie):
    """nav 接口验证 Cookie 并返回身份（is_login/uname/mid）"""
    try:
        data = bd.http_get_json(
            "https://api.bilibili.com/x/web-interface/nav", cookie=cookie)
    except Exception as e:
        return {"is_login": False, "uname": "", "mid": 0, "error": str(e)}
    d = data.get("data") or {}
    mid = d.get("mid") or 0
    is_login = bool(d.get("isLogin")) or (bool(mid) and mid != 0)
    return {"is_login": is_login,
            "uname": d.get("uname", "") if is_login else "",
            "mid": mid if is_login else 0}


def cmd_login_save(req_id, params) -> dict:
    """保存 Cookie（扫码轮询所得或手动粘贴），先经 nav 验证再持久化。
    写入 bili-digest 同一份凭据文件（~/.dsh/bilibili_credentials.json 等），
    与 DSH 插件共享登录态。"""
    cookie = str(params.get("cookie") or "").strip()
    if not cookie:
        raise ValueError("cookie 不能为空")
    if "SESSDATA" not in cookie:
        # 允许只贴 SESSDATA 值
        if "=" not in cookie and len(cookie) > 20:
            cookie = f"SESSDATA={cookie}"
        else:
            raise ValueError("Cookie 中缺少 SESSDATA 字段")
    if "buvid3=" not in cookie:
        cookie = f"buvid3={bd.make_buvid()}; {cookie}"

    _progress(req_id, "login_verify", "正在验证登录态（nav 接口）")
    ident = _nav_identity(cookie)
    if not ident.get("is_login"):
        return {"saved": False, "is_login": False, "uname": "", "mid": 0,
                "error": ident.get("error") or "Cookie 无效或已过期（nav 未确认登录）"}

    if not bd.save_cookie(cookie):
        return {"saved": False, "is_login": True, **ident,
                "error": "验证通过但写入凭据文件失败"}
    return {"saved": True, **ident}


def cmd_login_check(_req_id, _params) -> dict:
    """查询当前登录态"""
    cookie = bd.load_saved_cookie()
    ident = _nav_identity(cookie)
    return {**ident, "has_cookie": "SESSDATA" in (cookie or "")}


def cmd_logout(_req_id, _params) -> dict:
    """退出登录：清空共享凭据中的 Cookie"""
    ok = bd.save_cookie("")
    return {"logged_out": ok}


COMMANDS = {
    "ping": cmd_ping,
    "status": cmd_status,
    "classify": cmd_classify,
    "search": cmd_search,
    "digest": cmd_digest,
    "auto": cmd_auto,
    "batch": cmd_batch,
    "qr_start": cmd_qr_start,
    "qr_poll": cmd_qr_poll,
    "login_save": cmd_login_save,
    "login_check": cmd_login_check,
    "logout": cmd_logout,
}


# ---------------------------------------------------------------------------
# 调用日志（诊断用：记录每笔请求参数与落盘结果，写在脚本同目录）
# ---------------------------------------------------------------------------

_LOG_FILE = Path(__file__).resolve().parent / "bridge_calls.log"


def _log(msg: str) -> None:
    try:
        with open(_LOG_FILE, "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S") + " " + msg + "\n")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 主循环
# ---------------------------------------------------------------------------

def main() -> int:
    _emit({"event": "ready", "pid": os.getpid(), "commands": sorted(COMMANDS)})
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        req_id = None
        try:
            req = json.loads(line)
            req_id = req.get("id")
            cmd = str(req.get("cmd", "")).strip()
            params = req.get("params") or {}
            if not isinstance(params, dict):
                raise ValueError("params 必须是对象")
            handler = COMMANDS.get(cmd)
            if handler is None:
                raise ValueError(f"未知命令: {cmd}（可用: {sorted(COMMANDS)}）")
            _log(f"REQ id={req_id} cmd={cmd} params="
                 f"{json.dumps(params, ensure_ascii=False)[:500]}")
            t0 = time.time()
            data = handler(req_id, params)
            md = data.get("markdown_file") if isinstance(data, dict) else None
            _log(f"RES id={req_id} ok elapsed={int((time.time() - t0) * 1000)}ms "
                 f"markdown_file={md!r}")
            _emit({
                "id": req_id, "ok": True, "data": data,
                "elapsed_ms": int((time.time() - t0) * 1000),
            })
        except (json.JSONDecodeError, ValueError) as e:
            # 协议/参数错误：回复但继续运行
            _log(f"RES id={req_id} ERR(request) {e}")
            _emit({"id": req_id, "ok": False, "error": f"请求错误: {e}"})
        except Exception as e:  # 业务异常（网络/风控等）
            _log(f"RES id={req_id} ERR(business) {type(e).__name__}: {e}")
            _emit({
                "id": req_id, "ok": False,
                "error": f"{type(e).__name__}: {e}",
                "trace": traceback.format_exc(limit=3),
            })
    return 0


if __name__ == "__main__":
    sys.exit(main())
