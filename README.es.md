# XSOverlay en Linux (Steam/Proton) — puente de captura X11

XSOverlay funciona bajo Proton (la interfaz, el teclado, el wrist overlay...), pero
la captura de escritorio/ventanas sale transparente porque su plugin de captura
(`uWindowCapture.dll`) usa APIs de Windows (PrintWindow/BitBlt/Windows Graphics
Capture) que en Wine no ven el escritorio Linux.

Este proyecto sustituye ese plugin por una versión que habla con un helper nativo:

```
XSOverlay (Unity, Proton)                      Linux (host X11)
┌──────────────────────────────┐   /dev/shm    ┌─────────────────────────────┐
│ Assembly-CSharp (sin tocar)  │  xso-bridge   │ xso-bridge                  │
│   └─ P/Invoke Uwc*()         │ ◄──────────► │   XComposite + MIT-SHM      │
│ uWindowCapture.dll (nuestro) │  frames BGRA  │   XRandR (monitores+EDID)   │
│   └─ UpdateSubresource()     │  lista vent.  │   XFixes (cursor)           │
│      a la textura D3D11      │  eventos      │   XTest (ratón/teclado)     │
└──────────────────────────────┘               └─────────────────────────────┘
```

* `src/dll/uwc_linux.c` → `build/uWindowCapture.dll`: reimplementa los 83 exports
  del plugin original (incluidos los añadidos por XSOverlay: `UwcGetMonitorEdid`,
  `UwcGetDesktopUid`, `UwcIsPrimaryDesktop`). Lee la memoria compartida y sube los
  frames a las texturas de Unity desde el hilo de render. Además engancha
  `SendInput`/`SetCursorPos`/`SetForegroundWindow`/`ShowWindow` de user32 para
  reenviar el ratón y el teclado al escritorio X11.
* `src/helper/xso-bridge.c` → `build/xso-bridge`: enumera ventanas
  (`_NET_CLIENT_LIST_STACKING`), monitores (RandR, con EDID), captura ventanas con
  XComposite y el escritorio con XShmGetImage, lee el cursor con XFixes e inyecta
  entrada con XTest.
* `src/common/xso_shm.h`: protocolo de memoria compartida (archivo
  `/dev/shm/xso-bridge`, ~1.5 GiB *virtuales*, disperso).
* `xso-linux.sh`: wrapper de lanzamiento para Steam.
* `src/test/`: `shmtest` (cliente nativo) y `harness.exe` (simula Unity+D3D11 para
  probar el DLL con `proton run`, sin SteamVR).

## Instalación / uso

1. Compilar (ya hecho): `./build.sh` (necesita gcc, cabeceras X11, wine/winegcc, clang, lld).
2. En Steam → XSOverlay → Propiedades → Opciones de lanzamiento:

   ```
   /path/to/XSOverLainux/xso-linux.sh %command%
   ```

3. Compatibilidad: Proton Experimental (es lo que usaste en septiembre y funciona);
   Proton 9 / Proton Hotfix / GE-Proton10 también cargan el DLL correctamente.
4. **Arrancar SteamVR primero** y luego lanzar XSOverlay desde Steam (o dejar que
   SteamVR lo auto-arranque como overlay). Con Proton Experimental, si SteamVR no
   está corriendo XSOverlay recibe `Interface Not Found (105)`, reintenta 6 s y se
   queda sin VR; Proton 9 sí lanzaba SteamVR automáticamente (error 301).

Qué hace el wrapper en cada lanzamiento:

* arranca `xso-bridge` si no está corriendo (y lo para al salir),
* copia `build/uWindowCapture.dll` sobre el del juego (guarda el original como
  `uWindowCapture.dll.windows`; Steam lo restaurará al verificar/actualizar y el
  wrapper lo vuelve a instalar),
* lanza `XSOverlay.exe -batchmode` directamente en vez de
  `XSOverlay Process Manager.exe` (ese lanzador .NET muere con una
  NullReferenceException bajo Wine; `XSO_DIRECT=0` para no hacerlo).

Variables: `XSO_INPUT=0` (no reenviar ratón/teclado), `XSO_UWC_DEBUG=1` (log
detallado del DLL), `XSO_BRIDGE_ARGS="-vv"` (log por captura del helper),
`XSO_BRIDGE_SHM` (ruta del archivo compartido).

## Estado verificado (2026-10-24)

* `harness.exe` bajo Proton 9 + DXVK: lista de ventanas, iconos, cursor, captura de
  una ventana y del escritorio llegan a una textura D3D11 con los colores y la
  orientación correctos.
* XSOverlay real (Proton Experimental, SteamVR 2.18.4, Index): el plugin carga
  (`UnityPluginLoad`, device D3D11), XSOverlay recibe las 17 ventanas, y al crear un
  overlay por su API WebSocket (`Create`) pide capturas a 30 fps y las recibe
  (contador `captured` subiendo). Falta probarlo con el casco puesto.
* Hooks de entrada: probados en seco (`xso-bridge --dry-input`): posición absoluta,
  clics, rueda y teclas se decodifican bien. Falta probar en VR.
* Lanzado desde Steam con el wrapper (03:18): VR conectado, UI Chromium viva, la API
  responde con la lista de ventanas (incluido el escritorio) y el bucle del plugin
  sigue vivo (antes del arreglo de iconos se colgaba a los 5 s).

## Detalle importante: iconos obligatorios

XSOverlay solo mete una ventana en su lista de la interfaz cuando ha capturado su
icono (`IconCaptured`), y su rutina `UwcWindowList.UpdateWindowTitles` solo corta la
cadena de eventos `EventWindowListUpdated → OnRequestWindowList → UpdateWindowTitles`
al toparse con una entrada de escritorio. Si el escritorio no tiene icono nunca entra
en la lista y, en cuanto la UI se suscribe a `WindowList`, la recursión es infinita
(desbordamiento de pila → hilo principal muerto → "Fatal error in GC" a los minutos).
Por eso `xso-bridge` sintetiza un icono para los monitores (64x64) y para las ventanas
sin `_NET_WM_ICON` (48x48), y el DLL sube primero los escritorios.

## Limitaciones conocidas

* **Rendimiento**: la lectura del framebuffer en NVIDIA (XShmGetImage) cuesta
  ~25 ms a 2560x1440 con la GPU libre y 100–150 ms con VRChat + compositor de
  SteamVR cargando la GPU (≈7–10 fps en el overlay de escritorio). Ventanas más
  pequeñas van proporcionalmente más rápido. Mejoras posibles: escalar en el
  servidor X (XRender) antes de leer, o leer vía GLX `texture_from_pixmap` + PBO.
* El webview de la interfaz (Chromium de Vuplex) sólo arranca bien cuando XSOverlay
  se lanza **desde Steam** (dentro del contenedor Steam Linux Runtime). Lanzado a
  mano con `proton run` el proceso GPU de Chromium no arranca y la UI no aparece.
* Las capturas se fuerzan opacas (alfa=255); `xso-bridge --keep-alpha` conserva el
  alfa de ventanas ARGB.
* Teclado: las teclas se traducen VK→keysym para teclas comunes; símbolos raros
  pueden faltar. El estado de modificadores que XSOverlay lee con
  `GetAsyncKeyState` no refleja lo inyectado en X11.
* `XSOverlay Media Manager` (WinRT) falla al arrancar en Wine: inofensivo, sólo
  afecta a la info multimedia del wrist.
* `UwcGetWindowProcessId` devuelve el PID del propio XSOverlay para todas las
  ventanas (si no, `Process.GetProcessById` lanza excepción al guardar layouts).
* Sólo X11 (sesión actual). En Wayland habría que capturar con PipeWire/portal en el helper.

## Logs

* `<repo>/xso-linux.log` — wrapper
* `<repo>/xso-bridge.log` — helper
* `/dev/shm/xso-uwc.log` — DLL dentro de XSOverlay
* `.../compatdata/1173510/pfx/drive_c/users/steamuser/AppData/LocalLow/Xiexe/XSOverlay/output_logs/` — XSOverlay

## Volver al original

```
cd ~/.local/share/Steam/steamapps/common/XSOverlay_Beta/XSOverlay_Data/Plugins/x86_64
mv uWindowCapture.dll.windows uWindowCapture.dll
```

y quitar la opción de lanzamiento (o verificar archivos en Steam).

## Licencias / origen

API y semántica tomadas de hecomi/uWindowCapture (MIT). Código nuevo en este repo.
