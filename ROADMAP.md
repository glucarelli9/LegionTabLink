# LegionTabLink — Roadmap

## Fase 1: APK con UI de bienvenida y configuración

- Pantalla de bienvenida con logo/nombre de la app
- Estado visual: "Esperando conexión del servidor..." con animación
- Indicador de conexión (check verde al conectar)
- Una vez conectado, pantalla de configuración:
  - **Resolución**: dropdown (1920x1080, 2000x1200, 2560x1440, custom)
  - **FPS**: selector (30, 60, 90)
  - **Bitrate**: slider (10–50 Mbps)
  - **Codec**: HEVC (default), posible H.264 fallback
- Botón "Conectar" que inicia el streaming con la config elegida
- La config se envía al servidor via protocolo wire (extender packet Config type=2)

## Fase 2: Servidor Windows como .exe distribuible

- Empaquetar `host-streamer.exe` como portable o con installer (NSIS/Inno Setup)
- Auto-detectar tablets conectadas por USB (`adb devices`)
- Auto-setup de ADB reverse tunnels al detectar tablet
- Tray icon con estado de conexión (verde/rojo)
- Tray menu: Start/Stop streaming, ver tablet conectada, ajustar config
- Auto-instalar Virtual Display Driver si no existe (o bundlear)
- Auto-instalar ADB si no está en PATH

## Fase 3: Multi-tablet

- Soporte para múltiples tablets simultáneas
- Cada tablet = un monitor virtual separado
- El servidor crea N instancias de VDD + encoder + socket
- Cada tablet elige su propia resolución/fps/bitrate

## Notas técnicas

- Protocolo Config packet (type=2) debe extenderse para resolución/fps/bitrate del usuario
- El servidor debe poder re-inicializar el encoder con nueva config sin reiniciar
- APK necesita Jetpack Compose para UI real (actualmente es SurfaceView fullscreen)
- El .exe servidor necesita framework de UI (WinUI3, Qt, o Win32 tray icon)
- Para distribución: signing del APK, installer para el .exe
