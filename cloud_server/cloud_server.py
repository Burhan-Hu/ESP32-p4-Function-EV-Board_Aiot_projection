# -*- coding: utf-8 -*-
"""
智学Guardian 本地云平台服务器
================================

接收 ESP32 端 cloud_sync 组件上报的专注记录与安全报警，存 SQLite，
并提供浏览器控制台页面实时展示。纯 Python 标准库实现，零第三方依赖。

用法：
    python cloud_server.py [端口，默认5000]

首次启动后：
  1. Windows 防火墙弹窗请选择"允许访问"。
  2. 记下控制台打印的本机局域网 IP，填到 ESP32 工程 menuconfig 的
     "Cloud Sync (local cloud server) -> Cloud server base URL" 中，
     例如 http://192.168.137.1:5000 ，重新编译烧录。

部署到公网时：把本文件放到服务器运行，ESP32 把地址改成公网地址即可，
接口与数据格式不变。
"""
import json
import socket
import sqlite3
import sys
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

DB_PATH = "guardian_cloud.db"

ALERT_TYPE_LABELS = {
    "leave_seat": "疑似离座",
    "guard_alert": "环境异常",
}

# --------------------------------------------------------------------------
# Database
# --------------------------------------------------------------------------

_db_lock = threading.Lock()


def db_init():
    with _db_lock, sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """CREATE TABLE IF NOT EXISTS focus_sessions (
                   id INTEGER PRIMARY KEY AUTOINCREMENT,
                   name TEXT NOT NULL,
                   start_time INTEGER NOT NULL,
                   end_time INTEGER,
                   duration INTEGER,
                   created_at INTEGER NOT NULL
               )"""
        )
        conn.execute(
            """CREATE TABLE IF NOT EXISTS alerts (
                   id INTEGER PRIMARY KEY AUTOINCREMENT,
                   type TEXT NOT NULL,
                   confidence INTEGER NOT NULL DEFAULT 0,
                   event_time INTEGER NOT NULL,
                   detail TEXT NOT NULL DEFAULT '',
                   created_at INTEGER NOT NULL
               )"""
        )


def db_focus_start(name, start):
    with _db_lock, sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            "INSERT INTO focus_sessions (name, start_time, end_time, duration, created_at)"
            " VALUES (?, ?, NULL, NULL, ?)",
            (name, start, int(datetime.now().timestamp())),
        )


def db_focus_end(name, start, end, duration):
    with _db_lock, sqlite3.connect(DB_PATH) as conn:
        cur = conn.execute(
            "SELECT id FROM focus_sessions WHERE name = ? AND end_time IS NULL"
            " ORDER BY id DESC LIMIT 1",
            (name,),
        )
        row = cur.fetchone()
        if row:
            conn.execute(
                "UPDATE focus_sessions SET start_time = ?, end_time = ?, duration = ?"
                " WHERE id = ?",
                (start, end, duration, row[0]),
            )
        else:
            conn.execute(
                "INSERT INTO focus_sessions (name, start_time, end_time, duration, created_at)"
                " VALUES (?, ?, ?, ?, ?)",
                (name, start, end, duration, int(datetime.now().timestamp())),
            )


def db_alert(alert_type, confidence, event_time, detail):
    with _db_lock, sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            "INSERT INTO alerts (type, confidence, event_time, detail, created_at)"
            " VALUES (?, ?, ?, ?, ?)",
            (alert_type, confidence, event_time, detail,
             int(datetime.now().timestamp())),
        )


def db_records():
    with _db_lock, sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        focus = conn.execute(
            "SELECT name, start_time, end_time, duration FROM focus_sessions"
            " ORDER BY id DESC LIMIT 50"
        ).fetchall()
        alerts = conn.execute(
            "SELECT type, confidence, event_time, detail FROM alerts"
            " ORDER BY id DESC LIMIT 50"
        ).fetchall()
    return (
        [dict(r) for r in focus],
        [dict(r) for r in alerts],
    )


# --------------------------------------------------------------------------
# Web console page
# --------------------------------------------------------------------------

PAGE_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>智学Guardian 云平台</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: "Microsoft YaHei", "PingFang SC", sans-serif;
         background: #f0f2f5; color: #333; }
  header { background: linear-gradient(135deg, #1e3c72, #2a5298);
           color: #fff; padding: 18px 32px; display: flex;
           align-items: baseline; gap: 16px; }
  header h1 { font-size: 22px; }
  header span { font-size: 13px; opacity: .8; }
  .wrap { max-width: 1100px; margin: 24px auto; padding: 0 16px; }
  .card { background: #fff; border-radius: 10px; padding: 20px 24px;
          margin-bottom: 24px; box-shadow: 0 2px 8px rgba(0,0,0,.06); }
  .card h2 { font-size: 17px; color: #1e3c72; margin-bottom: 14px;
             border-left: 4px solid #2a5298; padding-left: 10px; }
  table { width: 100%; border-collapse: collapse; font-size: 14px; }
  th, td { padding: 9px 12px; text-align: left;
           border-bottom: 1px solid #eee; }
  th { background: #f7f9fc; color: #666; font-weight: 600; }
  tr.active td { background: #e8f7ee; }
  .badge { display: inline-block; padding: 2px 10px; border-radius: 10px;
           font-size: 12px; color: #fff; }
  .badge.leave_seat { background: #f0a020; }
  .badge.guard_alert { background: #d03050; }
  .badge.other { background: #888; }
  .running { color: #18a058; font-weight: 600; }
  .empty { color: #999; padding: 18px 12px; text-align: center; }
  .status { font-size: 12px; color: #999; text-align: right; }
</style>
</head>
<body>
<header>
  <h1>智学Guardian 云平台</h1>
  <span>网课专注守护 AIoT 终端 · 数据同步控制台</span>
</header>
<div class="wrap">
  <div class="card">
    <h2>专注学习记录</h2>
    <table id="focus">
      <thead><tr><th>姓名</th><th>开始时间</th><th>结束时间</th><th>本次时长</th><th>状态</th></tr></thead>
      <tbody></tbody>
    </table>
  </div>
  <div class="card">
    <h2>安全报警记录</h2>
    <table id="alerts">
      <thead><tr><th>时间</th><th>类型</th><th>置信度</th><th>详情</th></tr></thead>
      <tbody></tbody>
    </table>
  </div>
  <div class="status" id="status"></div>
</div>
<script>
const TYPE_LABELS = {leave_seat: "疑似离座", guard_alert: "环境异常"};
function fmt(ts) {
  if (!ts) return "-";
  const d = new Date(ts * 1000);
  const p = n => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ` +
         `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
function fmtDur(sec) {
  if (sec == null) return "-";
  const m = Math.floor(sec / 60), s = sec % 60;
  return `${String(m).padStart(2,"0")}:${String(s).padStart(2,"0")}`;
}
async function refresh() {
  try {
    const r = await fetch("/api/records");
    const data = await r.json();
    const fb = document.querySelector("#focus tbody");
    fb.innerHTML = "";
    if (!data.focus.length) {
      fb.innerHTML = '<tr><td colspan="5" class="empty">暂无专注记录，对设备说“开始学习”试试</td></tr>';
    }
    for (const f of data.focus) {
      const running = f.end_time == null;
      const tr = document.createElement("tr");
      if (running) tr.className = "active";
      tr.innerHTML = `<td>${f.name}</td><td>${fmt(f.start_time)}</td>` +
        `<td>${running ? "-" : fmt(f.end_time)}</td>` +
        `<td>${running ? "-" : fmtDur(f.duration)}</td>` +
        `<td>${running ? '<span class="running">● 进行中</span>' : "已完成"}</td>`;
      fb.appendChild(tr);
    }
    const ab = document.querySelector("#alerts tbody");
    ab.innerHTML = "";
    if (!data.alerts.length) {
      ab.innerHTML = '<tr><td colspan="4" class="empty">暂无报警记录</td></tr>';
    }
    for (const a of data.alerts) {
      const label = TYPE_LABELS[a.type] || a.type;
      const cls = TYPE_LABELS[a.type] ? a.type : "other";
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${fmt(a.event_time)}</td>` +
        `<td><span class="badge ${cls}">${label}</span></td>` +
        `<td>${a.confidence}%</td><td>${a.detail || ""}</td>`;
      ab.appendChild(tr);
    }
    document.getElementById("status").textContent =
      "最后刷新 " + new Date().toLocaleTimeString();
  } catch (e) {
    document.getElementById("status").textContent = "刷新失败: " + e;
  }
}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
"""

# --------------------------------------------------------------------------
# HTTP handler
# --------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    server_version = "GuardianCloud/1.0"

    def log_message(self, fmt, *args):  # quieter logs
        sys.stdout.write("[cloud] %s\n" % (fmt % args))

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html):
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            return json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    # ------------------------------ GET ------------------------------
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            self._send_html(PAGE_HTML)
        elif path == "/api/records":
            focus, alerts = db_records()
            self._send_json({"focus": focus, "alerts": alerts})
        else:
            self._send_json({"error": "not found"}, 404)

    # ------------------------------ POST ------------------------------
    def do_POST(self):
        path = urlparse(self.path).path
        data = self._read_json()
        if data is None:
            self._send_json({"error": "invalid json"}, 400)
            return

        if path == "/api/focus/start":
            name = str(data.get("name", "")).strip()
            start = int(data.get("start", 0))
            if not name or start <= 0:
                self._send_json({"error": "missing name/start"}, 400)
                return
            db_focus_start(name, start)
            print(f"[cloud] focus start: {name} @ {fmt_epoch(start)}")
            self._send_json({"ok": True})

        elif path == "/api/focus/end":
            name = str(data.get("name", "")).strip()
            start = int(data.get("start", 0))
            end = int(data.get("end", 0))
            duration = int(data.get("duration", 0))
            if not name or end <= 0:
                self._send_json({"error": "missing name/end"}, 400)
                return
            db_focus_end(name, start, end, duration)
            print(f"[cloud] focus end: {name} {fmt_epoch(start)} -> "
                  f"{fmt_epoch(end)} ({duration}s)")
            self._send_json({"ok": True})

        elif path == "/api/alert":
            alert_type = str(data.get("type", "unknown"))
            confidence = int(data.get("confidence", 0))
            event_time = int(data.get("time", 0))
            detail = str(data.get("detail", ""))[:200]
            if event_time <= 0:
                event_time = int(datetime.now().timestamp())
            db_alert(alert_type, confidence, event_time, detail)
            label = ALERT_TYPE_LABELS.get(alert_type, alert_type)
            print(f"[cloud] alert: {label} conf={confidence}% "
                  f"@ {fmt_epoch(event_time)} | {detail}")
            self._send_json({"ok": True})

        else:
            self._send_json({"error": "not found"}, 404)


def fmt_epoch(ts):
    return datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def get_lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    db_init()
    ip = get_lan_ip()
    print("=" * 56)
    print("  智学Guardian 本地云平台已启动")
    print(f"  本机局域网 IP : {ip}")
    print(f"  控制台地址    : http://{ip}:{port}/  (或 http://localhost:{port}/)")
    print(f"  ESP32 端配置  : menuconfig -> Cloud Sync -> Server URL")
    print(f"                  填 http://{ip}:{port}")
    print("  首次启动若弹出 Windows 防火墙提示，请选择“允许访问”")
    print("=" * 56)
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
