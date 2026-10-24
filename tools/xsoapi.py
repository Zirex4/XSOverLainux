import asyncio, json, sys, websockets
URL = "ws://127.0.0.1:42070/?client=xsolinux"
async def main():
    cmds = sys.argv[1:]
    async with websockets.connect(URL, max_size=16*1024*1024) as ws:
        for c in cmds:
            name, _, raw = c.partition('=')
            msg = {"sender": "xsolinux", "target": "xsoverlay", "command": name, "jsonData": "", "rawData": raw}
            await ws.send(json.dumps(msg)); print(">>", name, raw)
            try:
                while True:
                    r = await asyncio.wait_for(ws.recv(), timeout=2.0)
                    s = r if isinstance(r, str) else r.decode(errors='replace')
                    print("<<", s if __import__("os").environ.get("FULL") else s[:1500])
            except asyncio.TimeoutError:
                pass
asyncio.run(main())
