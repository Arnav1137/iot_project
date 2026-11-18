"""SQLite storage for Pala Lite."""
import json
import os
import sqlite3
import threading

DB_PATH = os.path.join(os.path.dirname(__file__), "data", "pala.db")
_lock = threading.Lock()


def _conn():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    c = sqlite3.connect(DB_PATH)
    c.row_factory = sqlite3.Row
    return c


def init():
    with _lock, _conn() as c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            type TEXT NOT NULL,              -- task | appointment | memory
            title TEXT, summary TEXT, transcript TEXT,
            due TEXT, start TEXT, tags TEXT DEFAULT '[]',
            done INTEGER DEFAULT 0,
            pending INTEGER DEFAULT 0,       -- 1 = AI processing failed, can reprocess
            audio_file TEXT, markers TEXT,
            created_at TEXT
        );
        CREATE TABLE IF NOT EXISTS qa (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            question TEXT, answer TEXT, audio_file TEXT, created_at TEXT
        );
        """)


def row(r):
    d = dict(r)
    if "tags" in d:
        try:
            d["tags"] = json.loads(d["tags"] or "[]")
        except Exception:
            d["tags"] = []
    return d


def add_item(it, transcript, audio_file, markers, created_at, pending=False):
    with _lock, _conn() as c:
        cur = c.execute(
            "INSERT INTO items(type,title,summary,transcript,due,start,tags,pending,audio_file,markers,created_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (it["type"], it["title"], it.get("summary", ""), transcript, it.get("due"), it.get("start"),
             json.dumps(it.get("tags", [])), int(pending), audio_file, markers, created_at))
        return cur.lastrowid


def get_item(item_id):
    with _conn() as c:
        r = c.execute("SELECT * FROM items WHERE id=?", (item_id,)).fetchone()
        return row(r) if r else None


def list_items(q="", type_=""):
    sql, args = "SELECT * FROM items WHERE 1=1", []
    if type_:
        sql += " AND type=?"; args.append(type_)
    if q:
        like = f"%{q}%"
        sql += " AND (title LIKE ? OR summary LIKE ? OR transcript LIKE ? OR tags LIKE ?)"
        args += [like] * 4
    sql += " ORDER BY created_at DESC, id DESC"
    with _conn() as c:
        return [row(r) for r in c.execute(sql, args).fetchall()]


def update_item(item_id, **fields):
    if not fields:
        return
    if "tags" in fields:
        fields["tags"] = json.dumps(fields["tags"])
    cols = ", ".join(f"{k}=?" for k in fields)
    with _lock, _conn() as c:
        c.execute(f"UPDATE items SET {cols} WHERE id=?", (*fields.values(), item_id))


def delete_item(item_id):
    with _lock, _conn() as c:
        c.execute("DELETE FROM items WHERE id=?", (item_id,))


def open_tasks():
    with _conn() as c:
        return [row(r) for r in c.execute(
            "SELECT * FROM items WHERE type='task' AND done=0 "
            "ORDER BY CASE WHEN due IS NULL THEN 1 ELSE 0 END, due, created_at").fetchall()]


def appointments_between(start_iso, end_iso):
    with _conn() as c:
        return [row(r) for r in c.execute(
            "SELECT * FROM items WHERE type='appointment' AND start>=? AND start<? ORDER BY start",
            (start_iso, end_iso)).fetchall()]


def recent_memories(limit=60):
    with _conn() as c:
        return [row(r) for r in c.execute(
            "SELECT * FROM items WHERE type='memory' AND pending=0 ORDER BY created_at DESC LIMIT ?",
            (limit,)).fetchall()]


def add_qa(question, answer, audio_file, created_at):
    with _lock, _conn() as c:
        c.execute("INSERT INTO qa(question,answer,audio_file,created_at) VALUES(?,?,?,?)",
                  (question, answer, audio_file, created_at))


def list_qa(limit=100):
    with _conn() as c:
        return [dict(r) for r in c.execute("SELECT * FROM qa ORDER BY id DESC LIMIT ?", (limit,)).fetchall()]
