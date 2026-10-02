#!/usr/bin/env python3
"""Deterministic log-like dataset for the n-gram LIKE / regex benchmark.

Writes CSV with header id,x,msg. msg is a log message body, x is uniform
in [0, 100) and independent of msg, so `x > 98/89/49/9` selects about
1/10/50/90% of rows.
"""

import argparse
import collections
import csv
import hashlib
import random
import sys

RESOURCES = ["users", "orders", "items", "carts", "sessions", "invoices",
             "products", "payments"]
NAMES = ["alice", "bob", "carol", "dave", "erin", "frank", "grace", "heidi",
         "ivan", "judy", "mallory", "niaj", "olivia", "peggy", "rupert",
         "sybil", "trent", "victor", "walter", "yuki"]
HOST_KINDS = [("db", 5432), ("cache", 6379), ("api", 8080), ("auth", 8443),
              ("queue", 5672)]
TABLES = ["users", "orders", "order_items", "payments", "audit_log",
          "sessions"]
JOBS = ["nightly_backup", "reindex_search", "cleanup_tmp", "send_digest",
        "rotate_logs", "sync_inventory"]
REASONS = ["card declined", "insufficient funds", "expired card",
           "fraud suspected"]
CLASSES = ["OrderService", "UserController", "PaymentGateway",
           "CartRepository", "SessionManager"]
METHODS = ["process", "handle", "validate", "load", "commit"]
# "oshibka avtorizatsii pol'zovatelya" in Cyrillic, kept as escapes
AUTH_ERROR_RU = ("\u043e\u0448\u0438\u0431\u043a\u0430 "
                 "\u0430\u0432\u0442\u043e\u0440\u0438\u0437\u0430\u0446\u0438\u0438 "
                 "\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f")


def api_version(r):
    return r.choices(["v1", "v2", "v3"], weights=[50, 35, 15])[0]


def millis(r):
    return int(r.expovariate(1 / 80)) + 1


def user(r):
    return f"{r.choice(NAMES)}_{r.randrange(10000):04d}"


def host(r):
    kind, port = r.choice(HOST_KINDS)
    return f"{kind}-{r.randrange(1, 21):02d}.prod.internal", port


def t_get(r):
    status = r.choices([200, 304, 404, 500], weights=[90, 4, 4, 2])[0]
    return (f"GET /api/{api_version(r)}/{r.choice(RESOURCES)}/"
            f"{r.randrange(1, 1000000)} {status} {millis(r)}ms")


def t_post(r):
    status = r.choices([201, 400, 409, 500], weights=[85, 8, 4, 3])[0]
    return (f"POST /api/{api_version(r)}/{r.choice(RESOURCES)} "
            f"{status} {millis(r)}ms")


def t_login(r):
    ip = (f"10.{r.randrange(256)}.{r.randrange(256)}."
          f"{r.randrange(1, 255)}")
    return f"user {user(r)} logged in from {ip}"


def t_logout(r):
    return f"user {user(r)} logged out after {r.randrange(1, 600)} minutes"


def t_cache(r):
    hit = r.choices(["hit", "miss"], weights=[80, 20])[0]
    return (f"cache {hit} for key {r.choice(RESOURCES)}:"
            f"{r.randrange(1, 1000000)}")


def t_timeout(r):
    h, port = host(r)
    return f"connection to {h}:{port} timed out after {millis(r) * 10}ms"


def t_retry(r):
    return (f"retrying request {r.getrandbits(32):08x} attempt "
            f"{r.randrange(1, 6)}/5")


def t_slow(r):
    return (f"slow query took {millis(r) * 20}ms: SELECT * FROM "
            f"{r.choice(TABLES)} WHERE id = {r.randrange(1, 1000000)}")


def t_disk(r):
    return (f"disk usage {r.randrange(1, 100)} pct on /dev/sd"
            f"{r.choice('abcd')}{r.randrange(1, 5)}")


def t_job(r):
    return f"scheduled job {r.choice(JOBS)} finished in {millis(r) * 50}ms"


def t_payment(r):
    return f"payment {r.getrandbits(48):012x} failed: {r.choice(REASONS)}"


def t_npe(r):
    cls = r.choice(CLASSES)
    return (f"java.lang.NullPointerException at com.example.{cls}."
            f"{r.choice(METHODS)}({cls}.java:{r.randrange(10, 900)})")


def t_oom(r):
    return "java.lang.OutOfMemoryError: Java heap space"


def t_auth_ru(r):
    return f"{AUTH_ERROR_RU} {user(r)}"


def t_heartbeat(r):
    h, _ = host(r)
    return (f"heartbeat from {h} seq={r.randrange(1, 10000000)} "
            f"load={r.random() * 4:.2f}")


# (name, generator, weight in percent)
TEMPLATES = [
    ("get", t_get, 30.0),
    ("post", t_post, 10.0),
    ("login", t_login, 10.0),
    ("logout", t_logout, 5.0),
    ("cache", t_cache, 8.0),
    ("timeout", t_timeout, 2.0),
    ("retry", t_retry, 3.0),
    ("slow", t_slow, 4.0),
    ("disk", t_disk, 5.0),
    ("job", t_job, 5.0),
    ("payment", t_payment, 1.0),
    ("npe", t_npe, 0.5),
    ("oom", t_oom, 0.05),
    ("auth_ru", t_auth_ru, 1.0),
    ("heartbeat", t_heartbeat, 15.45),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    r = random.Random(args.seed)
    names = [t[0] for t in TEMPLATES]
    gens = {t[0]: t[1] for t in TEMPLATES}
    weights = [t[2] for t in TEMPLATES]
    counts = collections.Counter()
    msg_bytes = 0

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "x", "msg"])
        for i in range(1, args.rows + 1):
            name = r.choices(names, weights=weights)[0]
            msg = gens[name](r)
            x = r.randrange(100)
            w.writerow([i, x, msg])
            counts[name] += 1
            msg_bytes += len(msg.encode("utf-8"))

    h = hashlib.sha256()
    with open(args.out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)

    print(f"python {sys.version.split()[0]} rows={args.rows} "
          f"seed={args.seed} msg_bytes={msg_bytes} "
          f"sha256={h.hexdigest()}", file=sys.stderr)
    for name in names:
        print(f"{name}\t{counts[name]}", file=sys.stderr)


if __name__ == "__main__":
    main()
