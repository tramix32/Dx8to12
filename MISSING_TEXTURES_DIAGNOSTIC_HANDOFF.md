# Handoff diagnostyczny: narastające „znikanie tekstur” w GTA Vice City

> ## ROZSTRZYGNIĘTE (2026-08-31)
>
> **Przyczyna**: GTA VC zapisuje **więcej danych indeksów, niż mieszczą jej
> własne statyczne bufory indeksów**, a następnie rysuje odwołując się do tych
> nadmiarowych indeksów. `Device::DrawIndexedPrimitive` klampował
> `index_count` do `resource_desc().Width / index_size`, wycinając końcowe
> trójkąty materiału — czyli dokładnie objaw „brakuje całego przebiegu”.
> Potwierdza to komentarz, który już był w `device.cpp` przy tym klampie
> („…exactly where a road material disappears, while the working D3D11 port
> submits the original count”), oraz `partialClamp` z pkt 7 tego dokumentu.
>
> **Dowód**: tymczasowa strefa ochronna za każdym buforem (build
> `DX8TO12_BUFFER_SHADOW`) wyłapała **219 różnych buforów** przelewających się
> poza koniec — wszystkie `kind=IB dynamic=0`, `flags=0x0`, przelanie zaczyna
> się na pierwszym bajcie za deklarowanym końcem, gra blokuje przy tym cały
> bufor (`lastLock=0+size`).
>
> **Poprawka**: `DX8TO12_PAD_BUFFERS` — `Buffer::InitAsBuffer` alokuje
> `AlignUp(Length,256) + 4096` i zeruje całość (upload heap ma nieokreśloną
> zawartość; bez zerowania zamienilibyśmy brakujące trójkąty na trójkąty ze
> śmieciowych indeksów). Nowe pole `d3d8_size_` przechowuje rozmiar **bez**
> paddingu i tylko ono trafia do `GetDesc`, więc gra paddingu nie widzi.
>
> **Weryfikacja**: `partialClamp=[]` / `partial:0` przez całą sesję (wcześniej
> setki na 30 klatek przy callsite `+0x278d8a`), sesja 46177 klatek — 4-5x
> dłuższa niż jakakolwiek wcześniejsza — bez dziur.
>
> **Obalone eksperymentalnie — nie wracać bez nowych danych**:
> sekcja A (zwykły `Lock` nie ma semantyki synchronizowanej / aliasing z już
> nagranym drawem) oraz sekcja B (ring → plain lock wybiera złą kopię).
> Sekcja A: zbudowano wierną kopię modelu `d3d8to11` (pełny CPU shadow, upload
> dirty range przez command listę) — **dziury pozostały**. Sekcja B: licznik
> na żywo pokazał **1 plain lock na 11643 klatki, zero hazardów** — VC
> praktycznie nie używa tej ścieżki.
>
> Otwarte: 4096 to wartość empirycznie wystarczająca, nie udowodniona granica
> (guard sprawdzał tylko pierwsze 64 bajty strefy). Koszt ~26 MB przy ~6400
> buforach. `DX8TO12_PAD_BUFFERS` jest na razie opt-in.

## Cel

Ustalić rzeczywistą przyczynę dziur pojawiających się podczas dłuższej gry z
`Dx8to12`, szczególnie przy 200–300 FPS. Nie zakładać z góry, że jest to wyciek
tekstur. Objaw wizualny wygląda jak brak tekstury drogi lub budynku, ale
dotychczasowe dane częściej wskazują na brak geometrii albo całego przebiegu
materiału.

Projekt jest obecnie mocno zmodyfikowany i ma niezacommitowane zmiany innych
agentów (m.in. per-pixel lighting, shader injection oraz DXR helper). Przed
pracą należy zapisać `git status --short` i `git diff --stat`. Nie wolno robić
`git reset`, `checkout` ani usuwać cudzych zmian.

## Środowisko i materiały porównawcze

- badany wrapper: `F:\Projekty\Dx8to12`
- gra: `F:\Gry\Grand Theft Auto Vice City Dev`
- działający port D3D8 -> D3D11: `F:\Projekty\d3d8to11`
- kod reVC: `F:\Projekty\ReVC\reVC`
- Ghidra: `F:\ghidra_12.1.3_PUBLIC`
- istniejący projekt Ghidry i stare logi: `analysis_work/` oraz `.analysis/`
- wcześniejszy plan obejścia LOD: `LOD_FALLBACK_ASI_PLAN.md`

Do testów wysokiego FPS używać `release-mindebug`, nie pełnego dev builda.
Pełna walidacja obniżała wydajność do około 10 FPS i potrafiła ukryć błąd
zależny od liczby klatek lub synchronizacji.

## Co wiadomo na pewno

1. Problem występuje częściej/szybciej przy wysokim FPS. Użytkownik zgłasza,
   że po dłuższej grze pojawia się coraz więcej dziur.
2. Z daleka nadal widać model LOD. Po zbliżeniu, gdy gra przechodzi na model
   bliski, fragment znika całkowicie.
3. W RenderDoc dla badanego fragmentu nie znaleziono jedynie błędnego sampla
   tekstury. Brakuje całego rysowania/przebiegu materiału: w uszkodzonym
   przypadku są dwa przebiegi zamiast trzech obserwowanych dla sąsiednich
   fragmentów drogi.
4. Samo czyszczenie cache, zmiany obsługi tekstur i wiele prób synchronizacji
   nie usunęły objawu. Ciężkie logowanie często zmieniało timing i utrudniało
   reprodukcję.
5. W `d3d8to11` problem według użytkownika nie występuje. Ten projekt należy
   traktować jako oracle zachowania D3D8, ale porównywać wejściowe wywołania
   API, ich HRESULT-y i parametry, a nie wyłącznie wewnętrzne API D3D11/D3D12.
6. W zebranym logu `analysis_work/log-rw-outcomes-20260827-231652.txt`
   wywołania `DrawIndexedPrimitive` zwykle miały `indexedAttempt ==
   indexedEmit`, `noIB=0`, `zeroClamp=0` i `prepareFail=0`. To wyklucza prosty
   scenariusz, w którym `Dx8to12` masowo wychodzi z funkcji przed nagraniem
   draw calla.
7. W tym samym logu po wejściu w określony obszar pojawiały się jednak setki
   lub ponad tysiąc `partialClamp` na 30 klatek, głównie dla callsite
   `gta-vc.exe+0x278d8a`, czasami również `+0x27442d` i `+0x27540d`.
   To jest istotny trop, lecz samo pojawienie się później w logu nie dowodzi
   narastania w czasie — gracz mógł po prostu wjechać do sceny, która używa
   takich buforów.
8. Wcześniejsza analiza rozpoznała m.in. pary near/LOD:

   | Near | LOD | Near name | LOD name |
   | ---: | ---: | --- | --- |
   | 4246 | 4260 | `nb_road02` | `LODroad02` |
   | 4248 | 4262 | `nb_road04` | `LODroad04` |
   | 4254 | 4268 | `nb_road10` | `LODroad10` |
   | 2060 | 2031 | `road_downtown15` | `LODd_downtown15` |

9. Utrzymywanie wybranego LOD-u jako eksperymentalnego fallbacku nie
   zatrzymało powstawania kolejnych dziur. Statyczna niezgodność siatki near i
   LOD może tłumaczyć wygląd pojedynczego miejsca, ale sama nie wyjaśnia
   narastającej liczby nowych miejsc.

## Wynik dodatkowego audytu statycznego (2026-08-31)

Najważniejsze nowe znalezisko nie dotyczy obiektu `GpuTexture`, lecz semantyki
zapisu VB/IB. Aktualny kod może zmienić pamięć, z której korzysta draw już
nagrany do tej samej command listy. To jest silniejszy trop niż ogólny
„wyciek tekstur” i dobrze pasuje do zależności od FPS oraz do sytuacji, w
której Pixel History nie widzi oczekiwanego przebiegu na danym pikselu: draw
może istnieć w liście zdarzeń, ale po podmianie danych jego trójkąty trafiają w
inne miejsce albo stają się degeneratami.

### A. Wysoki priorytet: zwykły `Lock` nie zapewnia semantyki synchronizowanej

`Buffer::Lock()` zwraca bezpośredni wskaźnik do stale zmapowanego upload heapu
(`resource()` przy obecnym `kBuffersInGpuMemory=false`). Nie czeka na GPU i nie
tworzy nowej wersji danych. Dotyczy to:

- wszystkich niedynamicznych VB/IB;
- `DynamicBuffer`, gdy `Lock` nie ma ani `D3DLOCK_DISCARD`, ani
  `D3DLOCK_NOOVERWRITE`;
- pierwszego `D3DLOCK_NOOVERWRITE` w nowej klatce, które obecnie również
  spada do `Buffer::Lock()`.

To nie odtwarza kontraktu zwykłego, synchronizowanego D3D8 `Lock`. Jeśli w tej
samej klatce wykonano już draw z tego bufora, a później gra zrobi zwykły
częściowy `Lock`, wcześniejszy draw nadal wskazuje ten sam GPU VA. Command list
nie została jeszcze wykonana, więc GPU może zobaczyć **nowe** bajty również dla
wcześniejszego drawa. Bariera zasobu tego nie naprawia — bariera porządkuje
operacje GPU, ale nie tworzy migawki pamięci zapisanej później przez CPU.

`DX8TO12_FORCE_GPU_IDLE` czeka dopiero po wysłaniu klatki. Usuwa overlap między
klatkami, ale **nie usuwa aliasowania wewnątrz jednej jeszcze niewysłanej
command listy**. Komentarz przy tym przełączniku, że wyklucza „every possible
CPU/GPU lifetime overlap”, jest więc zbyt mocny.

Działający `d3d8to11` rozwiązuje to inaczej: zarówno VB, jak i IB mają pełny,
żyjący przez cały czas CPU shadow; każdy `Lock` zwraca fragment tego shadow,
a `Unlock` wysyła dirty range przez `UpdateSubresource`. Sam komentarz w
`d3d8to11_vertex_buffer.cpp`/`d3d8to11_index_buffer.cpp` opisuje RenderWare
pakujący wiele siatek do wspólnych buforów i aktualizujący je częściowo. To
jest istotna różnica behawioralna między wrapperem działającym i wadliwym.

### B. Wysoki priorytet: mieszanie ringa i zwykłego `Lock` wybiera złą kopię

W `DynamicBuffer::Lock()` zwykły lock ustawia `is_plain_lock_` i zapisuje do
zasobu persistent przez `Buffer::Lock()`, ale nie unieważnia ani nie scala
`current_ring_alloc_`. Jeżeli ten sam bufor wcześniej w tej klatce przeszedł
przez `DISCARD`/`NOOVERWRITE`, to:

1. `prev_lock_frame_ == CurrentFrame()` i `current_ring_alloc_` nadal wskazuje
   ring;
2. zwykły lock zapisuje inne miejsce — persistent resource;
3. następny `GetGpuPtr()` nadal zwraca GPU VA ringa, więc draw nie widzi
   właśnie zapisanych danych;
4. `PersistDynamicChanges()` może na końcu klatki skopiować stare
   `written_ranges_` z ringa z powrotem do persistent resource i nadpisać
   zakres zmieniony zwykłym lockiem.

Sekwencja do wykrycia to co najmniej:

```text
ten sam Buffer i frame: DISCARD/NOOVERWRITE -> draw -> plain Lock -> draw
```

oraz wariant `DISCARD -> plain Lock -> pierwszy draw`. Obecne `LOCKDUMP` ma
potrzebne pola, ale uruchamia się tylko przy cięższym zrzucie UI. Dodać tani
licznik przejść trybu per bufor i logować wyłącznie pierwsze wystąpienie
`ring -> plain` wraz z offsetami, rozmiarami i callsite'em.

### C. Najlepszy test A/B dla A+B: pełny shadow/snapshot dla buforów

Zamiast kolejnej punktowej poprawki przygotować diagnostyczny przełącznik,
który świadomie naśladuje model z `d3d8to11`:

1. każdy VB/IB ma pełny CPU shadow zachowywany między lockami;
2. każdy `Lock`, niezależnie od flag, zwraca `shadow + OffsetToLock` i oznacza
   dirty range;
3. gdy draw potrzebuje danych zmienionych od poprzedniej migawki, alokować
   pełny zakres w ring bufferze i kopiować shadow; po kolejnym locku tworzyć
   **nową** migawkę, żeby wcześniejsze drawy w tej samej command list nie
   zmieniły znaczenia;
4. najnowszą wersję można na końcu klatki utrwalić w backing bufferze, ale nie
   wolno nadpisywać pamięci, którą wskazuje wcześniej nagrany draw.

Ten wariant będzie droższy, lecz ma być testem rozstrzygającym przy pełnym FPS.
Jeżeli dziury znikną, dopiero potem optymalizować osobno ścieżki
`DISCARD`/`NOOVERWRITE`. Samo dodanie `WaitForFrame` nie rozwiązuje aliasowania
dwóch drawów wewnątrz jednej command listy.

### D. Średni priorytet: błędne rozpoznawanie argumentu tekstury w shaderze FF

`GenerateArgValue()` poprawnie wybiera źródło przez
`arg & D3DTA_SELECTMASK`, natomiast `PixelShaderState` sprawdza brak tekstury
przez dokładne `arg == D3DTA_TEXTURE`. Przez to nie rozpoznaje:

- `D3DTA_TEXTURE | D3DTA_COMPLEMENT`;
- `D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE`;
- `COLORARG0`/`ALPHAARG0` używanych przez `D3DTOP_MULTIPLYADD` i
  `D3DTOP_LERP`;
- niejawnego użycia tekstury przez `D3DTOP_BLENDTEXTUREALPHA` i
  `D3DTOP_BLENDTEXTUREALPHAPM`.

Jednocześnie `SetTexture(stage, nullptr)` nie wiąże jawnego null SRV — pętla w
`PrepareDrawCall()` dla pustego `bound_textures_[stage]` niczego nie ustawia,
więc w root table pozostaje poprzedni deskryptor. Zwykle shader nie odczytuje
takiego slotu, ale przy powyższym błędzie generator może nadal wygenerować
sample i przeczytać stary/ponownie wykorzystany deskryptor. Naprawić analizę
argumentów przez `D3DTA_SELECTMASK`, uwzględnić Arg0 i operacje z niejawnym
samplem, a niezależnie dodać stały null SRV i wiązać go przy `SetTexture(NULL)`.
Flagi modyfikujące argument są legalnie łączone z wyborem źródła — potwierdza
to dokumentacja Microsoft `D3DTA`:
<https://learn.microsoft.com/windows/win32/direct3d9/d3dta>.

To jest realny błąd stale związanego SRV, ale ma niższy priorytet dla badanego
miejsca, ponieważ RenderDoc wskazywał przede wszystkim brak pokrycia/drawa, a
nie sample z niewłaściwej tekstury.

### E. Średni/niski priorytet: state block zapisuje „zmienione”, nie „ustawione”

`BeginStateBlock()` wykonuje pełny snapshot, a `EndStateBlock()` buduje blok
przez różnicę wartości przed/po. Prawdziwy state block ma zapamiętać stany,
których setter został wywołany podczas nagrywania, również gdy gra ustawiła
wartość równą bieżącej. Obecne redundant-set early-outy dodatkowo uniemożliwiają
odnotowanie takiego dotknięcia. Po późniejszym `ApplyStateBlock()` pominięty
stan może wyciekać z poprzedniego przebiegu.

To jest konkretny błąd implementacji, ale brak dowodu, że odpowiada za vanilla
Vice City: w kodzie reVC nie znaleziono użycia state blocków, a działający
`d3d8to11` ma te metody niezaimplementowane. Najpierw policzyć wywołania z
gry/modów. Jeśli licznik jest niezerowy, nagrywać maskę dotknięć bezpośrednio w
każdym `Set*`, zamiast wyliczać delta snapshotów. Dodatkowo
`CreateStateBlock(Type)` obecnie ignoruje podział `ALL/PIXELSTATE/VERTEXSTATE` i
przywraca wszystko, co także może clobberować niezwiązany stan moda.

### F. Ustalenia wykluczające część starszych hipotez

- `kCacheDrawStateBindings=false`: bieżący build ponownie ustawia root
  signature, CBV, tekstury, samplery i widoki na każdym drawie. Prosty błąd
  „cache pominął rebind” nie jest obecnie hipotezą pierwszego wyboru.
- Po resecie/mid-frame flush command listy kod ponownie wiąże oba descriptor
  heaps i zeruje cache IA/root/PSO. Ta wcześniej ryzykowna ścieżka wygląda w
  aktualnym kodzie spójnie.
- `kBuffersInGpuMemory=false`: nie analizować obecnie GPU-local staging path
  jako aktywnej ścieżki zwykłych VB/IB.
- `kSkipDiscardZeroFill=false`: nieużyty ogon po `DISCARD` jest zerowany, więc
  nie powinien sam z siebie zawierać losowych trójkątów z poprzedniej
  alokacji.
- `kDisableManagedResources=false`: managed textures zachowują CPU shadow.
- `DescriptorPoolHeap::Free()` ma odroczone ponowne użycie slotu do ukończenia
  odpowiedniego fence. Prosty descriptor use-after-free między klatkami został
  zaadresowany, choć nadal warto mierzyć zajętość puli.
- `GetAvailableTextureMem()` zwraca `UINT_MAX` zarówno tutaj, jak w
  `d3d8to11`, więc nie jest obecnie różnicą tłumaczącą wybór streamingu.

### G. Niespójność komentarza o niezależnym color/alpha chain

Generator w `ff_pixel_shader.cpp` ma nowy komentarz i pętlę traktującą color i
alpha jako niezależne po `ColorOp=DISABLE`, natomiast konstruktor
`PixelShaderState` nadal robi `break` przy wyłączonym color. Dokumentacja
Microsoft mówi, że wyłączenie color operation wyłącza dany stage i wszystkie
wyższe, efektywnie także alpha; wyłączenie tylko alpha przy aktywnym color daje
undefined behavior:
<https://learn.microsoft.com/windows/win32/direct3d9/d3dtextureop>.
Aktualny `break` jest więc bliższy dokumentowanej semantyce, a komentarz/pętla
w generatorze wprowadzają w błąd. Działający `d3d8to11` liczy stage aż oba opy
są wyłączone, ale to jego własne zachowanie, nie dowód specyfikacji. Nie
zmieniać tego w ciemno; najpierw zapisać realne stany MatFX badanego drawa.

## Najważniejsza zasada diagnozy

Nie nazywać tego błędem tekstury, dopóki istnieje draw call pokrywający
uszkodzony piksel i dopiero jego sampling jest błędny. Klasyfikacja ma być
następująca:

1. GTA nie dodaje encji do listy renderowania -> błąd widoczności/LOD/
   streamingu po stronie gry lub wpływ zwracanych caps/HRESULT-ów wrappera.
2. Encja jest na liście, ale RenderWare nie woła materiałowego D3D8 drawa ->
   wybór ścieżki MatFX, callback materiału albo stan RenderWare.
3. D3D8 draw wchodzi do `Dx8to12`, ale wrapper go pomija/truncuje -> błąd
   translacji D3D8.
4. D3D12 draw jest nagrany, lecz nie pokrywa piksela -> indeksy, wierzchołki,
   transformacja, culling, depth/alpha albo PSO.
5. D3D12 draw pokrywa piksel, ale próbka jest zła -> dopiero wtedy badać
   upload tekstury, SRV, deskryptor, mip i stan zasobu.

## Hipotezy w kolejności priorytetu

Po audycie z 2026-08-31 pierwszym testem implementacyjnym powinien być
`full-buffer shadow/snapshot` opisany w punkcie C powyżej. Numery poniższych
hipotez zachowano, żeby nie niszczyć wcześniejszych odwołań w notatkach; ich
stara numeracja nie oznacza już, że test MatFX ma wyprzedzać naprawę semantyki
VB/IB `Lock`.

### 1. Różna decyzja RenderWare MatFX / brak trzeciego przebiegu

To najlepiej pasuje do obserwacji „dwa przebiegi zamiast trzech”. W
`src/device.cpp` istnieje już lekka diagnostyka callsite'ów oparta o Ghidrę:

- `0x674380` — domyślny renderer materiału;
- `0x674510` — MatFX dual-pass;
- `0x674EE0` — environment map;
- `0x6756F0` — bump map;
- `0x676460` — główny callback mesha MatFX.

Kod loguje też globalne wartości gry `0x78A648` oraz `0x78A654`; komentarz w
kodzie wskazuje, że druga wartość bramkuje końcowy environment-map pass w
gałęzi bump+env. Należy porównać dokładnie tę samą drogę w dobrym i złym
momencie, a następnie te same wywołania w `d3d8to11`.

Szczególnie sprawdzić `ValidateDevice`. `Dx8to12` domyślnie zwraca sukces i
`pNumPasses=1`, natomiast `d3d8to11` ma tę metodę niezaimplementowaną i zwraca
błąd. Istnieje wariant CMake `DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL`, ale nie
uznawać wcześniejszego ogólnego testu za ostateczne wykluczenie. Trzeba
zarejestrować wszystkie wywołania `ValidateDevice`, aktywne texture-stage
states i wynik wyboru gałęzi MatFX dla konkretnej pary near/LOD.

Porównać także `GetDeviceCaps`, `CheckDeviceFormat`, `CheckDeviceType`,
`GetAvailableTextureMem`, liczbę texture stages i deklarowane możliwości
blendowania. Wrapper może powodować brak drawa po stronie gry nawet wtedy,
gdy sam `DrawIndexedPrimitive` działa poprawnie, ponieważ wcześniejszy wynik
capability check wybiera inną ścieżkę RenderWare.

### 2. Truncowanie indeksów w `Device::DrawIndexedPrimitive`

`Dx8to12` oblicza liczbę indeksów mieszczących się w związanym IB i domyślnie
zmniejsza `index_count`. `d3d8to11` przekazuje oryginalne `vertex_count`
bezpośrednio do `ID3D11DeviceContext::DrawIndexed`. W badanej scenie
`partialClamp` występował bardzo często i może usunąć ostatnie trójkąty
konkretnego materiału.

Nie wystarczy policzyć samych clampów. Dla każdego dotkniętego drawa zapisać:

- `PrimitiveType`, `MinIndex`, `NumVertices`, `StartIndex`, `PrimitiveCount`;
- format i rozmiar IB, oryginalny i wysłany index count;
- maksymalny indeks odczytany z rzeczywistych danych IB;
- `BaseVertexIndex`, stride i rozmiar VB;
- adres/ID obiektu `Buffer`, jego `content_generation` i dynamic/static;
- callsite GTA/RenderWare oraz teksturę/material stage 0.

Następnie zrobić kontrolowany A/B w identycznym miejscu i z tym samym save:

- zwykłe clamping;
- `DX8TO12_PASSTHROUGH_OOB_INDICES=ON`;
- bezpieczny wariant: utworzyć większy tymczasowy IB, skopiować poprawną
  część, a brakujący ogon uzupełnić indeksami degenerate zamiast czytać OOB.

Trzeci wariant rozdzieli „gra potrzebuje ogona” od „samo OOB destabilizuje
D3D12”. Surowy passthrough OOB jest niezdefiniowany w D3D12 i nie może być
docelową poprawką.

### 3. Dynamiczny VB/IB: DISCARD, NOOVERWRITE i ring buffer

W `src/buffer.cpp` były już realne błędy w częściowych lockach i persistowaniu
danych. Aktualny kod zawiera m.in. `PersistSpeculativeWrite`,
`IsRangeWrittenThisFrame`, `DebugCpuPtr`, `written_ranges_` oraz
`content_generation_`. Błąd rosnący z FPS może wynikać z ponownego użycia
alokacji ringa przed zakończeniem pracy GPU albo z czytania niewłaściwej
generacji danych.

Weryfikacja ma obejmować dane, nie tylko zakres:

- hash rzeczywistego span-u indeksów i referencjonowanych wierzchołków przy
  `Unlock` oraz bezpośrednio przed drawem;
- numer klatki locka, flagi `DISCARD/NOOVERWRITE`, offset i rozmiar;
- ring allocation: frame/offset/size/GPU VA;
- fence, po którym alokacja stała się ponownie dostępna;
- wykrycie nakładania się dwóch nadal żywych alokacji.

Po dodatkowym audycie nie ograniczać tego punktu do overlapu dwóch alokacji
ringa. Najbardziej podejrzane są teraz: zapis zwykłego `Lock` bez snapshotu do
zasobu już wskazanego przez wcześniejszy draw oraz sekwencja ring -> plain lock,
w której następny draw nadal wybiera ring. To są błędy możliwe nawet przy
idealnych fence'ach i wystarczająco dużym ringu.

Istnieje build `DX8TO12_FORCE_GPU_IDLE`. Jeśli w ściśle kontrolowanym teście
objaw nadal wystąpi przy oczekiwaniu po każdej klatce, race CPU/GPU i zbyt
wczesne ponowne użycie deskryptora/upload/ringa stają się mało prawdopodobne.
Nie wyklucza to jednak błędnych danych wytwarzanych już po stronie CPU.

### 4. Stan widoczności i streamingu GTA / przejście near-LOD

`CRenderer::SetupBigBuildingVisibility` ukrywa LOD po uznaniu near modelu za
gotowy i widoczny (`RwObject != nullptr`, widoczność osiąga 255). To może
tłumaczyć, dlaczego dziura ujawnia się dopiero po podejściu. Nie tłumaczy
jednak samodzielnie, dlaczego problem pojawia się wyłącznie z jednym
wrapperem ani dlaczego przybywa miejsc podczas sesji.

W reVC/Ghidrze instrumentować przejścia, a nie logować cały świat:

- `CRenderer::ConstructRenderList`, `ScanWorld`, `AddEntityToRenderList`;
- `CRenderer::SetupBigBuildingVisibility` i zwykłą widoczność encji;
- `CVisibilityPlugins` oraz material/atomic render callbacks;
- `CStreaming::RequestModel`, usuwanie modeli i zmianę statusu modelu;
- `CEntity::CreateRwObject` / `DeleteRwObject`;
- model ID, powiązany LOD, `RwObject`, visibility alpha i odległość.

W aktualnym `release-mindebug` istnieje `KeepGtaTargetRoadLodVisible()` wołane
z `BeginScene()`. Przed pomiarem bazowym należy je wyłączyć albo wystawić pod
osobny przełącznik. Modyfikuje stan gry, obejmuje tylko zapamiętaną parę i może
zanieczyścić wynik diagnostyki.

### 5. Narastanie cache/pamięci/deskryptorów

W projekcie istniał potwierdzony problem: nieznormalizowany klucz PSO potrafił
utworzyć ponad 75 000 obiektów D3D12 w długiej sesji, powodując spadek FPS i
crash. Obecny `CreatePSO` zeruje pola stanu, które nie wpływają na PSO, ale
zmiany shader injection/per-pixel lighting mogły ponownie zwiększyć liczbę
wariantów. `pso_cache_` i `ps_cache_` nie mają eviction, a przy zmianie trybu
lighting stare PSO celowo pozostają w cache.

Co 300–600 klatek zapisywać jeden kompaktowy rekord:

- rozmiar `pso_cache_`, `ps_cache_`, `sampler_cache_`;
- liczbę zajętych, wolnych i oczekujących SRV/RTV/DSV/sampler descriptors;
- rozmiary `frame_resources_to_free_` dla każdego slotu;
- high-water mark i liczbę żywych alokacji ring buffer;
- liczbę żywych `GpuTexture`, `Buffer` i shaderów;
- private bytes procesu oraz budżet/usage DXGI, jeśli dostępne.

Jeżeli licznik rośnie bez ograniczenia podczas stania w miejscu, najpierw
znaleźć właściciela przyrostu. Sam brak VRAM zwykle da błędny/nieutworzony
zasób lub device removal, a nie brak pojedynczego drawa, więc nadal należy
skorelować wzrost z konkretnym momentem zniknięcia przebiegu materiału.

### 6. SRV/tekstura — niższy priorytet

Badać dopiero, gdy RenderDoc potwierdzi, że odpowiedni draw istnieje i
pokrywa piksel. Wtedy sprawdzić:

- czy `SetTexture(0, ...)` dostało ten sam obiekt w dobrym i złym przypadku;
- slot deskryptora, generację alokacji i zawartość deskryptora;
- stan wszystkich subresources przed drawem;
- upload każdego mipa i hash CPU/GPU;
- czy pointer/descriptor ABA nie powoduje trafienia w stary cache;
- czy `PixelShaderState` nie wyłączył stage 0 przez błędną analizę używanych
  `COLORARG`/`ALPHAARG`.

Samo `tex0 != nullptr` nie dowodzi poprawności. Jednocześnie brak drawa w
Pixel History praktycznie wyklucza klasyczny „wyciek tekstury” jako przyczynę
tego konkretnego piksela.

## Test rozstrzygający, dlaczego problem narasta

Trzeba rozdzielić czas rzeczywisty, liczbę klatek i liczbę przejść LOD.
Przeprowadzić co najmniej poniższe próby z tego samego save i pozycji:

| Próba | Co robić | Interpretacja |
| --- | --- | --- |
| A | Stać nieruchomo 10 minut przy 30 i 300 FPS | Ten sam numer klatki awarii wskazuje stan aktualizowany per-frame; ten sam czas wskazuje timer/streaming/asynchroniczne zwalnianie. |
| B | Wielokrotnie przekraczać jedną granicę near/LOD | Awaria po podobnej liczbie przejść wskazuje nieodwracalny błąd transition/refcount/state. |
| C | Latać po mapie bez wracania | Przyrost tylko przy nowych sektorach wskazuje streaming, tworzenie zasobów lub cache per-content. |
| D | Stać w jednym uszkodzonym miejscu | Dalsze pojawianie się dziur bez streamingu wskazuje per-frame state/cache/race. |
| E | Zapisać/wczytać grę lub teleportować się daleko i wrócić | Powrót geometrii wskazuje stan gry/streamingu; brak powrotu — trwały cache/resource wrappera. |
| F | Alt-Tab/Reset device bez przeładowania save | Naprawa po resecie wskazuje zasoby/deskryptory/PSO wrappera. |

Każdy wynik zapisywać równocześnie w sekundach i numerze `Present`, nie jako
„po chwili”. Wysoki FPS może tylko szybciej wykonywać wadliwą operację
per-frame; nie jest automatycznie dowodem race GPU.

## Zalecany recorder dobry/zły

Nie pisać pliku na każdym drawie. Użyć pierścienia w pamięci obejmującego
ostatnie 120–300 klatek i flush tylko po hotkeyu:

- F8: oznacz ostatnią dobrą klatkę;
- F9: oznacz pierwszą złą klatkę i zapisz pierścień;
- zapis binarny lub zwarte rekordy, bez `std::ostringstream` na hot path;
- osobny rosnący `draw_sequence` na klatkę.

Minimalny rekord drawa:

```text
frame, qpc, thread, gtaCallerRva, rwBranch,
primitiveType, minIndex, numVertices, startIndex, primitiveCount,
originalIndexCount, submittedIndexCount,
vbPtr, vbGeneration, vbSize, vbStride, vbGpuVA, vbHash,
ibPtr, ibGeneration, ibSize, ibFormat, ibGpuVA, ibHash,
baseVertex, maxReferencedIndex,
worldHash, vsId, psId, psoId,
tex0Ptr, tex0SrvIndex, tex0Generation,
renderTarget, depthTarget, cull, zEnable, zWrite, alphaTest, alphaBlend
```

Równolegle mały hook po stronie GTA powinien emitować dla interesującej encji:

```text
frame, modelId, lodModelId, entityPtr, rwObjectPtr,
streamState, visibility, distance, addedToRenderList,
atomicCallbackEntered, materialCallbackEntered
```

Łączenie rekordów po numerze klatki i kolejności pokaże pierwszy punkt, w
którym dobra i zła ścieżka się rozchodzą.

## Porównanie z d3d8to11

Najbardziej wartościowy test to identyczny lekki recorder na granicy API
D3D8 w obu wrapperach. Porównywać:

1. kolejność i parametry `SetIndices`, `SetStreamSource`, `SetTexture`,
   `SetTextureStageState`, `SetRenderState` i `DrawIndexedPrimitive`;
2. wszystkie HRESULT-y zwracane do gry;
3. wyniki capability queries oraz `ValidateDevice`;
4. flagi i zakresy `Lock/Unlock` dynamicznych VB/IB;
5. czy trzeci MatFX draw w ogóle dochodzi do wrappera.

Jeżeli wejściowy ślad D3D8 różni się przed brakującym drawem, przyczyny
szukać w wcześniejszym HRESULT/caps/timingu. Jeżeli ślad jest identyczny, a
różni się dopiero wynik D3D12, przyczyna jest w `Dx8to12`.

Uwaga: `d3d8to11` jest rozbudowany i ma własne mody renderingu. Do porównania
najlepiej wyłączyć jego dodatkowe efekty i zostawić możliwie czystą translację.

## Mody zewnętrzne

W procesie były widoczne m.in. Mod Loader, Limit Adjuster, SilentPatch,
VCTrainer12, CLEO i inne ASI. Przynajmniej jeden pełny test wykonać z minimalną
instalacją: gra + jeden wrapper + dodatki bezwzględnie potrzebne do startu.
Limit Adjuster może relokować tablice streamingu, a mod renderujący przez
callback może zmieniać timing i stan D3D12. Nie należy obwiniać żadnego moda
bez A/B, ale pełny stos uniemożliwia czyste przypisanie winy.

## Czego nie powtarzać bez nowej informacji

- Nie dodawać kolejnego ogólnego logu `CreateTexture/Release/SetTexture`.
- Nie zakładać, że widoczny LOD oznacza uszkodzony mip tekstury near.
- Nie robić wielogigabajtowego trace każdego wywołania — zmienia timing.
- Nie wymuszać globalnie wszystkich LOD-ów; spowoduje z-fighting i ukryje
  symptom zamiast wskazać warstwę błędu.
- Nie traktować pojedynczego RenderDoc capture jako testu wysokiego FPS;
  capture i warstwa debug zmieniają harmonogram CPU/GPU.
- Nie poprawiać kilku hipotez w jednym buildzie A/B.
- Nie uznawać `indexedAttempt == indexedEmit` za dowód poprawnego drawa:
  draw może mieć skrócone indeksy, złe dane, zły PSO albo zostać odrzucony
  przez depth/culling.

## Kolejność pracy dla następnego agenta

1. Wyłączyć/gate'ować `KeepGtaTargetRoadLodVisible()` i zachować obecne zmiany.
2. Wyłączyć `LightingMode >= 2`, x64 RT helper, shader injection i callbacki
   overlay na czas bazowego testu. Nie mogą wpływać na ścieżkę, której nie są
   częścią.
3. Wybrać jeden stale odtwarzalny fragment, najlepiej `nb_road02`/`LODroad02`.
4. Zbudować diagnostyczny pełny CPU shadow + immutable snapshot per wersja dla
   wszystkich VB/IB. To obecnie najważniejszy A/B względem modelu działającego
   `d3d8to11`.
5. Równolegle dodać bardzo tani licznik sekwencji `ring -> plain Lock` i
   zwykłych locków wykonanych po drawie tego samego bufora w tej samej klatce.
6. Dodać recorder pierścieniowy dobry/zły oraz hook listy renderowania GTA.
7. Ustalić, czy brakujący trzeci pass nie jest wywoływany przez
   RenderWare, czy ginie dopiero w `Dx8to12`.
8. Jeśli nie wchodzi do wrappera: porównać `ValidateDevice`, caps i MatFX
   globals/callbacki z `d3d8to11`.
9. Jeśli wchodzi: zbadać clamp, dane VB/IB, transformację i PSO.
10. Naprawić maskowanie `D3DTA_SELECTMASK`/Arg0/implicit texture ops i wiązać
    null SRV; testować jako osobny build, nie razem z shadow-buffer A/B.
11. Dodać wolne liczniki cache/deskryptorów co 600 klatek, żeby
   rozstrzygnąć rzeczywiste narastanie zasobów.
12. Dopiero po wskazaniu pierwszej rozbieżnej funkcji przygotować pojedynczą
   poprawkę i test 30/60/144/300 FPS.

## Kryterium zakończenia diagnozy

Diagnoza jest zakończona dopiero wtedy, gdy dla tego samego modelu i miejsca
istnieje para dobry/zły z jednoznacznie wskazanym pierwszym rozjazdem, np.:

- encja przestaje trafiać do `ms_aVisibleEntityPtrs`;
- callback MatFX pomija trzeci pass po konkretnym wyniku `ValidateDevice`;
- D3D8 wywołuje draw, ale `Dx8to12` zmniejsza count z X do Y i usuwa
  trójkąty pokrywające badaną drogę;
- zwykły `Lock` po wcześniejszym drawie zmienia hash bajtów widzianych przez
  ten wcześniejszy draw, bo oba wskazują ten sam upload-heap GPU VA;
- po sekwencji `DISCARD/NOOVERWRITE -> plain Lock` CPU zapisuje backing
  resource, ale następny draw nadal pobiera GPU VA z `current_ring_alloc_`;
- hash dynamicznego VB/IB zmienia się między `Unlock` i drawem;
- draw jest poprawny, lecz wskazuje ponownie użyty deskryptor SRV.

Samo „po zmianie wygląda lepiej” nie jest jeszcze ustaleniem przyczyny.
