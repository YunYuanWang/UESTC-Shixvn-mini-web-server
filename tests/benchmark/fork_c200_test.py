#!/usr/bin/env python3
"""Test fork c=200 reproducibility - 3 runs to check if anomaly is consistent."""
import subprocess, socket, time, threading, os, signal

def kill_server():
    subprocess.run(["pkill", "-9", "-f", "mini_web_server"], capture_output=True)
    time.sleep(0.3)

def test(port, N, timeout=8):
    proc = subprocess.Popen(["./mini_web_server", "fork", "127.0.0.1", str(port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    results = []; lock = threading.Lock()
    def req(i):
        try:
            s = socket.socket(); s.settimeout(timeout)
            s.connect(('127.0.0.1', port))
            t0 = time.time()
            s.sendall(b'GET /hello HTTP/1.0\r\n\r\n')
            d = s.recv(4096)
            with lock: results.append(('ok', round((time.time()-t0)*1000, 1)))
        except Exception as e:
            with lock: results.append(('fail', str(e)[:50]))

    ts = [threading.Thread(target=req, args=(i,)) for i in range(N)]
    for t in ts: t.start()
    time.sleep(timeout)

    ok = sum(1 for r in results if r[0]=='ok')
    fail = N - ok
    lats = [r[1] for r in results if r[0]=='ok' and isinstance(r[1], (int,float))]

    # Count zombies
    try:
        out = subprocess.check_output("ps aux | grep defunct | grep -v grep | wc -l", shell=True).decode().strip()
        zombies = int(out)
    except: zombies = -1

    proc.send_signal(signal.SIGKILL)
    proc.wait()
    time.sleep(0.5)

    return ok, fail, lats, zombies

os.chdir("/home/zigh-wang/data-disk/shixvn/miniwebserver")
for run in range(1, 4):
    kill_server()
    subprocess.run(["rm", "-f", "logs/server.log"])
    ok, fail, lats, zombies = test(9995, 200)
    avg = sum(lats)/len(lats) if lats else 0
    p95 = sorted(lats)[len(lats)*95//100] if lats else 0
    print(f"run{run}: c=200 -> {ok}/200 ok, {fail} fail, avg={avg:.1f}ms, P95={p95:.1f}ms, zombies={zombies}")

kill_server()
print("done")