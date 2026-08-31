# Dx8to12 → VCVisual12: podsumowanie sesji (2026-08-29)

## ✅ Gotowe i działające — możesz używać już teraz

**Poziom 1 — metadane sceny** (`src/dx8to12_api.cpp`, opisane w MODDING.md
sekcja "Scene metadata for mods"):

- `Dx8to12_RequestDepthBufferAccess(true)` + `Dx8to12_GetDepthBufferSrv` /
  `GetDepthBufferSrvGpuHandle` / `GetDepthBufferFormat` — dostęp do bufora
  głębi bieżącej klatki. **Ważne**: opt-in, i **jedna klatka opóźnienia** —
  jeśli włączasz to z poziomu własnego callbacku po raz pierwszy, tranzycja
  stanu zadziała dopiero od następnej klatki, nie natychmiast.
- `Dx8to12_GetViewProjMatrix` — macierz view*proj, row-major (konwencja
  D3D8/`D3DMATRIX`).
- `Dx8to12_GetActiveLightCount` / `GetActiveLight` + struct
  `Dx8to12_LightInfo` (pełne odwzorowanie pól `D3DLIGHT8`, nie tylko
  okrojony podzbiór).

**Poziom 3 — wstrzykiwanie pixel shadera** (MODDING.md sekcja "Pixel
shader injection"):

- `Dx8to12_RegisterPixelShaderInjection` / `UnregisterPixelShaderInjection`
  + `Dx8to12_InvalidatePixelShaderCache`.
- Punkt wstrzyknięcia jest **po** bloku per-pixel lighting, nie zaraz po
  deklaracji `diffuse_color`/`specular_color` — widzisz finalne, oświetlone
  wartości.
- Fallback na błąd kompilacji: jeśli Twój fragment HLSL się nie
  skompiluje, Dx8to12 loguje błąd i renderuje bez wstrzyknięcia — reszta
  gry działa dalej normalnie, sprawdzaj `log.txt` pod kątem
  `Dx8to12_PixelShaderInjectionFn produced HLSL that failed to compile`.
- `has_normal`/`has_view_pos` w kontekście to **hint, nie gwarancja** —
  permutacja shadera jest cache'owana per-kształt state'u, nie per-draw,
  więc te flagi odzwierciedlają pierwszy draw który wywołał kompilację, nie
  każdy draw który potem użyje tego samego shadera. Nie polegaj na nich
  dla poprawności, tylko jako zgrubna optymalizacja — strażuj
  `IN.oViewNormal` runtime'owo tak jak robi to `ComputeLighting`.

**Naprawiony bug współdzielenia sterty deskryptorów**: jeśli Twój
callback (H4 composite) bindował własną stertę SRV (żeby zbudować widok
na pożyczony zasób H4), a potem inny mod (np. VCTrainer12) renderował po
Tobie w tej samej klatce i nie rebindował defensywnie — dziedziczył Twoją
stertę i się wywalał/renderował śmieci. **Teraz Dx8to12 sam rebinduje
własną stertę przed każdym zarejestrowanym callbackiem** — więc każdy mod
zawsze startuje ze znanego stanu, niezależnie od kolejności rejestracji i
tego co zrobił poprzedni mod. Nie musisz nic zmieniać, to już działa.

## ❌ Jeszcze NIE gotowe — kanał H4 (prawdziwe cienie RT)

**Kontrakt API się nie zmienił** — `Dx8to12_GetRtShadowOutputResource` /
`GetRtShadowDoneFence` / `GetRtShadowDoneFenceValue` /
`GetRtShadowOutputWidth/Height/Format` istnieją i działają dokładnie tak
jak opisane w MODDING.md. Ale **realny dispatch promieni cieni jest
obecnie wyłączony** (`kEnableTlasAndDispatch = false` po stronie x64
helpera) — kanał zwraca placeholder 1×1/`UNKNOWN`, tak jak zawsze. Twój
kod sprawdzający wymiary przed rysowaniem (zgodnie z ustaleniem: przez
eksporty, nie `GetDesc()`) powinien więc dalej poprawnie **nie rysować**
— to oczekiwane, nie bug.

**Dlaczego wyłączone**: 4 próby włączenia dispatchu w tej sesji, wszystkie
skończyły się `DXGI_ERROR_DRIVER_INTERNAL_ERROR` (usunięcie urządzenia na
współdzielonym adapterze x86/x64 NVIDIA) plus jeden pełny crash systemu
przy okazji live-debuggingu. Po drodze naprawione dwa realne bugi po
stronie helpera (przepełnienie `VertexCount` w BLAS, `InstanceMask=0`
blokujący wszystkie testy promieni), ale problem nie jest jeszcze w pełni
zdiagnozowany. Szczegóły w pamięci projektu Dx8to12
(`dx8to12-mod-api-and-h3`), gdyby ktoś chciał kontynuować.

**Co to znaczy dla Ciebie**: nic nie musisz robić po swojej stronie. Gdy
H4 zacznie realnie działać (osobna, przyszła sesja po stronie Dx8to12),
wymiary zaczną się zgadzać z rozdzielczością okna i Twój istniejący kod
compositingu zadziała automatycznie — dokładnie tak jak było zaplanowane
od początku.
