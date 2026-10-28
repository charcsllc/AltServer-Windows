# AltServer para Windows: error -80046 — investigación, estado y plan

Fecha: 28 de octubre de 2026
Repositorio: `charcsllc/AltServer-Windows`, rama **`fix-80046`** (basada en `arm_1.7.5_jayce`, AltServer 1.7.5 de Riley Testut)
Release de prueba: https://github.com/charcsllc/AltServer-Windows/releases/tag/v1.8.1

---

## 1. El problema

Al iniciar sesión, AltStore o AltServer en Windows muestran:

> Failed to Log In — This action cannot be completed at this time. (-80046)

- Empezó alrededor del **21–22 de octubre de 2026**, por un **cambio de Apple**. No es un problema de tu cuenta ni de tu contraseña.
- Afecta a casi todas las herramientas de sideloading (AltStore, SideStore, Sideloadly, Impactor, xtool…). AltStore lo reconoce oficialmente para **AltServer en Windows** ([issue altstoreio/AltStore#1840](https://github.com/altstoreio/AltStore/issues/1840)) y, a la fecha, **no hay arreglo oficial**.
- La contraseña y el 2FA **sí funcionan**. Lo que falla es el último paso: Apple se niega a entregar el **token de Xcode** (`com.apple.gs.xcode.auth`, petición `apptokens`).

## 2. Causa técnica

Apple ahora comprueba **desde qué tipo de equipo** se inicia sesión. Eso lo indican dos cosas:

- La **identidad declarada**, en el encabezado `X-Mme-Client-Info`: Mac, Windows, Android…
- Los **datos de identificación del equipo** ("anisette": `X-Apple-I-MD`, `X-Apple-I-MD-M`…). AltServer para Windows los genera con **iCloud para Windows**.

Lo que descubrimos probando:

| Datos de identificación | Identidad declarada | Resultado |
|---|---|---|
| iCloud para Windows | Windows (AltServer oficial 1.7.x/1.8.x) | **-80046**: Apple no da el token de Xcode a un PC |
| iCloud para Windows | Mac (nuestra 1.ª versión 1.8.1) | **-22410**: Apple lo rechaza por incoherente (es el mismo error que Riley arregló en febrero de 2026) |
| **Generados en una Mac real** | **Mac** | Es lo que usan iloader, xtool e iPASide, **que sí funcionan** |

**Conclusión:** con iCloud para Windows no hay salida. Hacen falta **datos de identificación de Mac**.

## 3. La solución elegida: servidores "anisette v3"

Hay servidores públicos que generan datos de identificación **en una Mac real** con el protocolo anisette v3 (el de SideStore):

1. `https://mac-ani.jkcoxson.com` (el que usan xtool, iloader y atvloadly)
2. `https://new.ani.sidestore.io`
3. `https://ani3.stikstore.app`

Funcionamiento del protocolo, **ya probado de principio a fin con los tres servidores** mediante un prototipo en Python:

1. `GET /v3/client_info` → devuelve la identidad de Mac (`<MacBookPro13,3> <Mac OS X;10.12.6;16G29> <com.apple.AuthKit/1 (com.apple.akd/1.0)>`) y el User-Agent.
2. Se registra el PC una sola vez (WebSocket `/v3/provisioning_session`):
   - El servidor pide un identificador (16 bytes aleatorios) y luego datos `spim`/`cpim`.
   - AltServer los intercambia con Apple: `gsa.apple.com/grandslam/MidService/startMachineProvisioning` y `.../finishMachineProvisioning`.
   - El servidor devuelve `adi_pb`, que se guarda.
3. Cada vez que hacen falta datos: `POST /v3/get_headers` con `{identifier, adi_pb}` → `X-Apple-I-MD`, `X-Apple-I-MD-M`, `X-Apple-I-MD-RINFO`.

**Privacidad:** el servidor ve tu IP y el registro del PC, pero **nunca** tu Apple ID, tu contraseña ni tus códigos.

**Ventaja extra:** la app AltStore del iPhone **le pide estos datos a AltServer**. Por eso también podría dejar de mostrar "Failed to Log In" en el iPhone sin recompilar AltStore.

## 4. Lo que ya está hecho (rama `fix-80046`)

| Cambio | Archivos |
|---|---|
| Proveedor remoto de datos de identificación (anisette v3): WebSocket con WinHTTP, HTTP con cpprestsdk, estado en `C:\ProgramData\AltServer\Anisette.json` | `AltServer/RemoteAnisetteProvider.{h,cpp}`, `AltServer/AnisetteDataManager.cpp` |
| Variables opcionales: `ALTSERVER_ANISETTE_SERVER=<url>` (otro servidor) y `ALTSERVER_ANISETTE=local` (volver a iCloud) | igual |
| Versión 1.8.1 | `AltServer/Resource.rc`, `AltInstaller/AltInstaller.vdproj` |
| **Workflow de GitHub Actions** que compila AltServer (Win32, Visual Studio 2026) y publica el release | `.github/workflows/build-windows.yml` |
| cpprestsdk (ya no existe en vcpkg), añadido como *overlay port* y parcheado para VS 2026 (`stdext::checked_array_iterator`, `_Openprot`) | `.github/vcpkg-ports/cpprestsdk/` |
| OpenSSL 3: silenciar funciones obsoletas (`/sdl` las convierte en error) | `Directory.Build.props` |
| Rutas de íconos que apuntaban al PC de Riley | `AltServer/Resource.rc` |

El paquete compila y trae todas sus DLL (comprobado con el análisis de dependencias).

## 5. Estado actual: error de certificado SSL

Al probar la última versión salió:

> AltServer could not fetch anisette data from any anisette server.
> SSL Error: WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED … WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA

**Qué significa:**
- **Windows no confía en el certificado de los servidores** (`INVALID_CA`) y **no puede comprobar si fue revocado** (`CERT_REV_FAILED`).
- Los certificados son correctos: Let's Encrypt (ISRG Root X1/X2) y Google Trust Services (GlobalSign Root CA). Los revisé desde aquí.
- El fallo es del **almacén de certificados de tu Windows**. Causas probables:
  - La **actualización automática de certificados raíz** está desactivada o bloqueada. Algunas herramientas de "privacidad" o "debloat" lo hacen. Chrome y Edge usan su propio almacén, así que el navegador funciona aunque Windows no tenga esos certificados.
  - Un **antivirus o proxy que intercepta HTTPS**.
- cpprestsdk además **obliga a comprobar la revocación** (`http_client_winhttp.cpp`, línea ~1092: `WINHTTP_ENABLE_SSL_REVOCATION`), y cualquier fallo de red al consultar las CRL hace fallar la conexión.
- El código original de AltSign evita esto **desactivando la validación** con `gsa.apple.com`. Yo no quiero desactivarla.

### Diagnóstico que puedes hacer tú (PowerShell)
```powershell
Invoke-WebRequest https://mac-ani.jkcoxson.com/v3/client_info
reg query "HKLM\SOFTWARE\Policies\Microsoft\SystemCertificates\AuthRoot" /v DisableRootAutoUpdate
```
- Si el primer comando también da error de certificado, se confirma que es el almacén de Windows.
- Si el segundo devuelve `0x1`, la actualización de raíces está desactivada.
- Arreglo manual posible: borrar esa clave, o importar los certificados raíz:
  ```powershell
  certutil -generateSSTFromWU roots.sst
  ```
  y luego abrir `roots.sst` e importarlo.

## 6. Plan para el error SSL (siguiente paso)

**Actualización (28/10):** en tu PC, `Invoke-WebRequest` conectó sin problema y `DisableRootAutoUpdate` no existe. Por tanto Windows sí confía en los certificados, y el fallo es solo la **comprobación de revocación que fuerza cpprestsdk**. Solución aplicada:
- Las peticiones a los servidores anisette usan **WinHTTP directamente**, con la validación normal de Windows (autoridad, nombre y fecha) y sin exigir la revocación, igual que PowerShell.
- Las de registro con `gsa.apple.com` relajan solo la autoridad, como ya hace AltSign.

Si aun así fallara, lo de abajo queda como plan B.

Hacer que AltServer **verifique los certificados por su cuenta, sin depender del almacén de Windows ni de la revocación**, sin desactivar la seguridad:

1. Hacer las peticiones a los servidores anisette y a `gsa.apple.com` (registro del PC) con un pequeño cliente **WinHTTP propio**, no con cpprestsdk.
2. Pedirle a WinHTTP que no valide la autoridad (`SECURITY_FLAG_IGNORE_UNKNOWN_CA`) ni la revocación, pero **validar a mano antes de enviar ningún dato**:
   - `WinHttpSendRequest` sin cuerpo → `WINHTTP_OPTION_SERVER_CERT_CONTEXT`.
   - `CertGetCertificateChain` con un almacén **exclusivo** que solo contiene raíces incluidas en AltServer: ISRG Root X1, ISRG Root X2, GlobalSign Root CA, GTS Root R1–R4 y Apple Root CA.
   - `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)` con el nombre del servidor e ignorando solo "revocación desconocida".
   - Si todo está bien, `WinHttpWriteData` con el cuerpo → `WinHttpReceiveResponse`.
3. Si aun así falla, el mensaje dirá claramente que algo intercepta HTTPS en tu PC (por ejemplo, el antivirus), para que lo desactives.
4. **Alternativa rápida y menos segura:** desactivar la validación como ya hace AltSign con Apple. No la recomiendo.

Esfuerzo estimado: unas 150 líneas en `RemoteAnisetteProvider.cpp` y 1–2 compilaciones en GitHub Actions.

## 7. Cómo instalar y probar el build
1. Descargar `AltServer-1.8.1-win32.zip` del release.
2. Cerrar AltServer ("Quit") y hacer copia de `C:\Program Files (x86)\AltServer`.
3. Copiar encima todos los archivos del zip.
4. Abrir AltServer y usar **Install AltStore**. Hace falta iCloud e iTunes de la web de Apple, no de Microsoft Store.
5. **No aceptar actualizaciones** de AltServer: reemplazarían este build.

## 8. Contexto para continuar en otra sesión
- **Repositorios:** `charcsllc/AltServer-Windows` (rama `fix-80046`), `charcsllc/AltSign` (rama `marketplace`, Swift, la que usa la app del iPhone) y `charcsllc/AltStore`.
- **Compilación:** se hace con GitHub Actions; cada push a `fix-80046` compila y reemplaza el release `v1.8.1`.
- **Código clave:**
  - Inicio de sesión: `AltSign/AppleAPI+Authentication.cpp`.
  - Datos de identificación: `AltServer/AnisetteDataManager.cpp` y `AltServer/RemoteAnisetteProvider.cpp`.
  - La app del iPhone pide los datos en `AltServer/ClientConnection.cpp` (`ProcessAnisetteDataRequest`).
- **Fuentes:**
  - https://blog.illixion.com/2026/10/sideloading-error-80046/
  - https://github.com/altstoreio/AltStore/issues/1840
  - https://newreleases.io/project/github/pwnapplehat/iPASide/release/v1.2.9
