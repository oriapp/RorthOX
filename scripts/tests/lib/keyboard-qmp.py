"""Send an acknowledged key event or an atomic three-event keyboard burst."""
import json
import socket
import sys

path, action, key = sys.argv[1:4]
if action == "down":
    keys = [(key, True)]
elif action == "burst" and len(sys.argv) == 5:
    keys = [(sys.argv[4], False), (key, True), (key, False)]
else:
    raise SystemExit("usage: keyboard-qmp.py SOCKET down KEY | SOCKET burst KEY PREVIOUS")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.settimeout(10)
    sock.connect(path)
    stream = sock.makefile("rb")
    greeting = json.loads(stream.readline())
    if "QMP" not in greeting:
        raise RuntimeError(f"not a QMP greeting: {greeting}")

    def command(name, arguments):
        sock.sendall((json.dumps({"execute": name, "arguments": arguments}) + "\n").encode())
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError("QMP closed before acknowledging command")
            reply = json.loads(line)
            if "error" in reply:
                raise RuntimeError(f"{name}: {reply}")
            if "return" in reply:
                print(name, json.dumps(arguments), json.dumps(reply), flush=True)
                return

    command("qmp_capabilities", {})
    command("input-send-event", {"events": [
        {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}
        for name, down in keys
    ]})
