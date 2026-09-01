# Handoff: DLSS / DLAA i przygotowanie pod DLSS 5 przez istniejacy helper x64

## Cel

Rozszerzyc istniejacy `dx8to12_rt_helper.exe` zamiast tworzyc kolejny proces.
Helper ma zostac ogolnym hostem funkcji NVIDIA x64 (Streamline/NGX), podczas
gdy GTA VC i `d3d8.dll` pozostaja x86.

Najblizszy realny kamien milowy to **DLAA albo DLSS Super Resolution przez
helper x64**. Wlasciwego DLSS 5 nie nalezy jeszcze implementowac na podstawie
domyslow: na dzien 2026-09-01 publiczna dokumentacja integracyjna i publiczny
SDK DLSS 5 nie sa dostepne. Projekt mozna natomiast przygotowac tak, aby po
publikacji SDK dolozenie nowych wejsc nie wymagalo ponownego projektowania IPC.

## Stan projektu potwierdzony w kodzie

Istniejacy helper rozwiazuje juz problem architektury procesu:

- `d3d8.dll` dziala jako x86, a `rt_helper/dx8to12_rt_helper.exe` jest osobnym
  targetem x64;
- helper wybiera ten sam adapter za pomoca LUID;
- helper tworzy wlasne urzadzenie i kolejke D3D12 oraz wykrywa DXR;
- `RtHelperClient::BeginSmokeTest()` potwierdzil otwieranie wspoldzielonego
  bufora D3D12 oraz synchronizacje dwoma wspoldzielonymi fence'ami pomiedzy
  x86 i x64;
- istnieje wersjonowany, pozbawiony wskaznikow protokol IPC w
  `shared/rt_ipc_protocol.h`;
- helper ma kontrolowany lifecycle i job z `KILL_ON_JOB_CLOSE`.

Istniejacy helper **nie jest jeszcze hostem DLSS**:

- `rt_helper/CMakeLists.txt` linkuje tylko DXGI, D3D12 i DXCompiler; nie ma
  Streamline ani NGX;
- protokol v14 przesyla geometrie sceny przez ograniczony do 8 MiB payload
  CPU;
- produkcyjny wynik RT jest kopiowany przez `shadow_payload` CPU i wgrywany
  do x86-lokalnej tekstury 320x180;
- wspoldzielony bufor/fence w `BeginSmokeTest()` jest tylko jednorazowym
  testem, nie transportem klatek;
- nie istnieja pelnowymiarowe wspoldzielone tekstury color/depth/motion/output;
- renderer nie generuje motion vectors ani jittera projekcji;
- callback moda w `Device::Present()` widzi finalna klatke razem z HUD-em.

Komentarze w `rt_ipc_protocol.h` wspominajace stary wspoldzielony prototyp H4
nie oznaczaja, ze aktualna produkcyjna sciezka wyniku jest GPU-shared.
Autorytatywny jest kod `RtHelperClient::ShadowOutputResources`,
`RecordShadowUpload()` i `shadow_done_fence()`: aktualnie zasob jest lokalny
dla x86, upload idzie z mapped payload, a accessor fence zwraca `nullptr`.

## Docelowa architektura

```text
GTA VC x86
  -> Dx8to12 x86
       -> render sceny do scene_color/depth/motion
       -> transition wspoldzielonych wejsc do COMMON
       -> queue Signal(input_ready, frame_value)
                              |
                              v
       NvidiaFeatureHost x64 (obecny rt_helper)
       -> queue Wait(input_ready, frame_value)
       -> Streamline/NGX EvaluateFeature
       -> wynik do wspoldzielonego output_color
       -> transition output do COMMON
       -> queue Signal(output_ready, frame_value)
                              |
                              v
       Dx8to12 queue Wait(output_ready, frame_value)
       -> copy/composite wyniku
       -> HUD w rozdzielczosci wyjsciowej
       -> Present
```

CPU events sluza tylko do sterowania i wybudzania helpera. Poprawnosc dostepu
GPU ma opierac sie na shared D3D12 fences. Nie robic CPU wait w `Present()`.

Uzyc 2 lub 3 slotow klatek. Jeden pojedynczy zestaw zasobow wymusilby
serializacje obu procesow i moglby powodowac duzy spadek FPS.

## Zasoby wymagane dla pierwszego DLSS SR/DLAA

Kazdy slot klatki powinien moc rejestrowac co najmniej:

| Zasob | Sugerowany format poczatkowy | Uwagi |
|---|---|---|
| `input_color` | `R16G16B16A16_FLOAT` albo format zaakceptowany przez SDK | scena bez HUD-u |
| `depth` | obecny typeless depth z kompatybilnym widokiem | ten sam depth, z ktorego wynikaja motion vectors |
| `motion_vectors` | `R16G16_FLOAT` | gesty bufor, kamera i obiekty dynamiczne |
| `output_color` | format zaakceptowany przez SDK i kompozycje | rozdzielczosc wyjsciowa |
| `exposure` | 1x1, zgodnie z SDK | na poczatku mozna uzyc auto-exposure |
| `reactive_mask` | opcjonalny 1-kanalowy | woda, ogien, czasteczki, alfa |
| `transparency_mask` | opcjonalny 1-kanalowy | poprawa stabilnosci przezroczystosci |

Nie zakladac z gory formatow dla nieopublikowanego DLSS 5. Protokol powinien
przesylac tablice opisow zasobow: semantyka, format, width, height, slot,
generation i nazwa shared handle. Pozwoli to dodac normals, roughness, albedo
lub inne bufory bez kolejnej niekompatybilnej przebudowy calej struktury.

## Rozszerzenie IPC

Dodac nowa wersje protokolu i osobny channel DLSS, nie przeciagac kolejnych
pol przez pola `shadow_*`. Kazda zmiana layoutu wymaga podbicia `kVersion`.

Control plane powinien zawierac:

- feature requested/available i wersje pluginow;
- numer slotu oraz monotoniczny `frame_id`;
- input/output width i height;
- tryb DLAA/Quality/Balanced/Performance/Ultra Performance;
- nazwy shared resources i obu shared fences;
- wartosci `input_ready_value` oraz `output_ready_value`;
- jitter w pixel space;
- render subrect/extent;
- current/previous view, projection i view-projection bez jittera;
- near/far, FOV i aspect;
- `depthInverted`, `cameraMotionIncluded`, `reset`, `cameraCut`;
- skale motion vectors;
- status/HRESULT/SL result i krotki komunikat diagnostyczny;
- generation zwiazana z Reset/swapchainem.

Przez granice x86/x64 nadal nie wolno przesylac wskaznikow, `size_t`, GPU VA,
descriptor handles ani surowych wartosci `HANDLE`. Przesylac nazwy obiektow
NT, stale typy liczbowe i identyfikatory.

## Prace wymagane po stronie Dx8to12 x86

Helper nie wygeneruje po fakcie prawidlowych danych temporalnych. Nalezy
rozbudowac glowny renderer:

1. **Historia transformacji**
   - zachowac aktualne i poprzednie view/projection;
   - wykrywac camera cut, teleport, load, Reset i zmiane rozdzielczosci;
   - dla ruchomych obiektow zachowac poprzedni world transform;
   - zaprojektowac stabilny klucz obiektu/drawa. Sama kolejnosc draw calli nie
     jest stabilna i nie moze byc jedynym kluczem.

2. **Motion vectors**
   - rozszerzyc fixed-function i programmable VS o previous clip position;
   - dodac velocity render target i MRT PSO zamiast obecnego jednego RTV;
   - pretransformed `XYZRHW`/UI nie powinno generowac motion vectors sceny;
   - prototyp moze zaczac od camera-only velocity rekonstruowanego z depth,
     ale pojazdy i piesi beda wtedy smuzyc.

3. **Jitter**
   - dodac sekwencje temporalna, np. Halton;
   - wstrzykiwac jitter do projekcji geometrii 3D;
   - nie jitterowac HUD-u ani `XYZRHW`;
   - do Streamline przekazywac macierze bez jittera oraz jitter osobno.

4. **Oddzielenie sceny od HUD-u**
   - obecny callback przed `Present` jest za pozno, bo zawiera HUD;
   - przekierowac swiat do osobnego `scene_color`, uruchomic DLSS, a HUD
     narysowac pozniej w rozdzielczosci wyjsciowej;
   - klasyfikacje UI oprzec m.in. o `XYZRHW`, depth state, projection,
     blending i znane draw patterns; dodac diagnostyczny podglad klasyfikacji;
   - nie zmieniac zachowania offscreen render targets gry.

5. **Rozdzielczosc renderowania**
   - najpierw uruchomic DLAA 1:1, bez zmiany rozdzielczosci sceny;
   - dopiero po poprawnym obrazie temporalnym wprowadzic nizsze render size;
   - viewport/scissor/depth musza odpowiadac input extent, backbuffer i HUD
     pozostaja w output extent.

## Prace wymagane w helperze x64

1. Dodac SDK Streamline/NGX jako zewnetrzna, jawnie dostarczona zaleznosc.
   Nie pobierac ani nie commitowac produkcyjnych binariow bez sprawdzenia
   licencji/redystrybucji.
2. Zainicjalizowac Streamline przed utworzeniem urzadzenia albo zastosowac
   oficjalna sciezke manual hooking dla istniejacego urzadzenia.
3. Otwierac named shared textures/fences na urzadzeniu tego samego LUID.
4. Utrzymywac per-slot command allocator/list i poprawny fence lifecycle.
5. Wywolac feature support/query settings i zwrocic zalecane input resolution.
6. Tagowac color input/output, depth, motion vectors oraz opcjonalne maski.
7. Ustawic per-frame constants i wykonac DLSS evaluate.
8. Po wyniku przywrocic uzgodniony stan `COMMON` i sygnalizowac output fence.
9. Przy timeout, device removed albo crashu helpera natychmiast przejsc po
   stronie x86 na zwykly raster/PerPixel. Gra nie moze czekac bez limitu.
10. Logowac wersje Streamline/NGX, feature availability, wybrany preset,
    rozmiary, frame/slot/fence oraz wynik evaluate bez logowania co draw.

## Frame Generation

Nie laczyc pierwszej implementacji SR/DLAA z Frame Generation. DLSS-G wymaga
gestych motion vectors, depth, HUD-less color, danych UI, Reflex i integracji
z prezentacja/swapchainem. Obecny swapchain i `Present()` sa w procesie x86,
a Streamline dziala w helperze x64. Samo udostepnienie tekstur nie daje
helperowi kontroli nad Present.

Najpierw zakonczyc DLAA/SR. Dla FG przeprowadzic osobny proof-of-concept:
helper musialby prawdopodobnie stac sie wlascicielem finalnego swapchaina i
prezentacji w oknie gry albo nalezaloby znalezc oficjalnie wspierana sciezke
Streamline, ktora nie wymaga interposera w procesie prezentujacym. Nie zakladac,
ze cross-process FG zadziala bez osobnej walidacji.

RTX 4080 obsluguje zwykle Frame Generation, ale nie nalezy utozsamiac tego z
Multi Frame Generation przeznaczonym dla generacji Blackwell/RTX 50.

## Faktyczne DLSS 5

Na dzien tego dokumentu NVIDIA oglosila DLSS 5 jako nowy model neuronowego
renderowania oswietlenia i materialow, ale nie opublikowala publicznego SDK ani
kontraktu wejsc. Nie wymyslac typow G-bufferow i nie nazywac dzialajacego DLSS
SR "DLSS 5" tylko na podstawie numeru DLL/modelu.

Architekture przygotowac przez:

- elastyczna rejestracje semantyk zasobow;
- wersjonowanie capabilities;
- wiele opcjonalnych wejsc per frame;
- formaty i extents zapisane w IPC;
- niezalezne feature IDs oraz feature-specific constants blob o stalej,
  jawnie opisanej wersji;
- brak zalozenia, ze DLSS 5 bedzie tylko kolejnym presetem DLSS SR.

Po publikacji SDK nalezy porownac oficjalne wymagania z dostepnymi danymi GTA
VC. Jezeli DLSS 5 bedzie wymagal fizycznych materialow, rozdzielonych skladowych
oswietlenia albo bogatego G-bufferu, fixed-function D3D8 nie dostarczy ich
automatycznie. Trzeba bedzie je odtworzyc/wyprowadzic w shaderach Dx8to12.

## Kolejnosc implementacji i kryteria odbioru

### D0 - capability host

- helper buduje sie z SDK x64;
- wypisuje wersje SDK/pluginow i wynik sprawdzenia DLSS;
- brak SDK lub brak wsparcia nie blokuje uruchomienia gry.

### D1 - pelnowymiarowy GPU round-trip bez DLSS

- x86 udostepnia dwie/trzy tekstury slotow i fences;
- x64 kopiuje lub modyfikuje `input_color` do `output_color` bez CPU payload;
- wynik 2560x1440 jest poprawny przez kilka minut przy wysokim FPS;
- Reset, alt-tab, zmiana rozdzielczosci i zakonczenie gry nie zostawiaja
  helpera ani zawieszonych fence waits;
- brak stalego wzrostu VRAM/RAM i brak crasha/device removed.

### D2 - DLAA proof-of-concept

- input i output maja te sama rozdzielczosc;
- depth, camera-only motion vectors, jitter i constants przechodza walidacje;
- mozna przelaczac Off/DLAA bez restartu i poprawnie resetowac historie;
- HUD jest tymczasowo wylaczony albo wyraznie wykluczony z oceny obrazu.

### D3 - poprawne dane temporalne

- motion vectors obejmuja kamere i obiekty dynamiczne;
- brak silnego ghostingu samochodow/pieszych;
- camera cuts, teleporty, menu i loading resetuja historie;
- dostepny jest debug view velocity/depth/jitter/UI classification.

### D4 - rozdzielenie sceny i UI

- DLSS dostaje scene bez HUD-u;
- radar, napisy, menu i overlay sa komponowane po DLSS w output resolution;
- offscreen render targets i efekty GTA nadal dzialaja.

### D5 - Super Resolution

- render size wynika z query settings SDK;
- Quality/Balanced/Performance dzialaja bez zmiany rozmiaru okna;
- wynik jest stabilny w ruchu i po Reset;
- awaria/helper timeout automatycznie wraca do natywnego renderingu.

### D6 - przygotowanie/implementacja DLSS 5

- zaczac dopiero po uzyskaniu oficjalnego SDK i programming guide;
- zaktualizowac ten dokument rzeczywistymi wymaganiami;
- dodac brakujace material/lighting buffers etapami, z osobnymi debug views.

## Punkty kodu do rozpoczecia pracy

- `rt_helper/rt_helper_main.cpp` - urzadzenie x64, petla IPC i DXR;
- `rt_helper/CMakeLists.txt` - zaleznosci helpera;
- `shared/rt_ipc_protocol.h` - obecny protokol v14;
- `src/rt_helper_client.cpp` - lifecycle, smoke shared resource/fence,
  CPU transport wyniku;
- `src/rt_helper_client.h` - publiczny stan helpera;
- `src/device.cpp`, `Device::PrepareDrawCall()` - macierze i PSO per draw;
- `src/device.cpp`, `Device::Present()` - submission, callbacks i Present;
- `src/device.cpp`, `Device::CreatePSO()` - obecnie jeden render target;
- `src/ff_pixel_shader.cpp` oraz `src/shaders/` - wyjscia shaderow;
- `MODDING.md` - aktualny kontrakt API dla modow.

## Oficjalne materialy, ktore nalezy traktowac jako zrodlo prawdy

- DLSS SR: <https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md>
- Streamline general: <https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md>
- Frame Generation: <https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md>
- Streamline repo: <https://github.com/NVIDIA-RTX/Streamline>
- publiczny DLSS SDK: <https://github.com/NVIDIA/DLSS>
- ogloszenie DLSS 5: <https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/>

Przed implementacja zawsze sprawdzic aktualna wersje tych dokumentow. Nie
opierac kontraktu zasobow na blogach, nazwach pobranych DLL ani przypuszczeniach.

## Bezpieczenstwo zmian

Repozytorium zawiera duzo niezacommitowanych zmian innych agentow. Nie
przepisywac ani nie cofac obcych modyfikacji. Najpierw zrobic D0/D1 jako
izolowany kanal obok obecnego RT v14. Dopiero po stabilnym tescie kilku minut
na wysokim FPS laczyc kanal DLSS z glownym render targetem.

Problem znikajacych tekstur i wczesniejsze `device removed` nie sa dowodem
bledu DLSS, ale musza pozostac na uwadze: temporalny upscaler zwiekszy liczbe
zasobow, barier i synchronizacji. Kazdy etap ma miec kontrolowane limity VRAM,
poprawny teardown oraz test wielokrotnego uruchomienia gry.
