# tools/

Scripts used to reverse-engineer XSOverlay and to drive it. Python ones expect a venv:

```bash
python3 -m venv .venv && . .venv/bin/activate && pip install dnfile dncil websockets
A=~/.local/share/Steam/steamapps/common/XSOverlay_Beta/XSOverlay_Data/Managed/Assembly-CSharp.dll
```

| Script | Purpose |
|---|---|
| `dump_pinvoke.py A` | every P/Invoke in the assembly with decoded signatures (`dll!entry -> ret Name(params)`) |
| `dump_types.py A Name...` | fields/methods of the named types (`~substr` for substring match) |
| `dump_marshal.py A Name...` | `FieldMarshal` blobs (e.g. `ByValTStr` sizes) and `ClassLayout` of struct types |
| `dump_il.py A Name...` | IL of every method that calls the named methods/P/Invokes (`fld:Name` finds field uses) |
| `dump_body.py A Type::Method...` | IL of specific methods (`--full` for every instruction) |
| `dump_eh.py A Method...` | exception handler ranges of methods (is there a try/catch?) |
| `xsoapi.py Cmd[=rawData]...` | WebSocket client for XSOverlay's API on 127.0.0.1:42070 (`FULL=1` prints whole replies) |
| `ppm2png.py in.ppm out.png` | convert the PPM dumps written by `shmtest`/`harness.exe` |
| `bench.c` | `XShmGetImage` throughput on root and on a composite pixmap |

`dnutil.py` holds the shared signature decoder.
