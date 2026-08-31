# Plan moda ASI: fallback LOD dla dróg Vice City

## Cel

Stworzyć osobny mod ASI dla GTA Vice City 1.0, niezależny od `d3d8.dll` i
`dx8to12`, który zapobiega dziurom nawierzchni podczas przejścia z dalekiego
modelu LOD na model bliski.

Mod nie zmienia plików gry ani modeli DFF/TXD. Działa wyłącznie w pamięci
procesu i może zostać wyłączony przez usunięcie pliku ASI.

## Ustalenia z diagnozy

- Problem nie jest utratą tekstury ani wyciekiem zasobów GPU.
- Bliski model drogi jest załadowany, ma `RwObject` i wysyła draw-calle.
- Część modeli bliskich dróg nie pokrywa w pełni obszaru pokrywanego przez ich
  model LOD.
- `CRenderer::SetupBigBuildingVisibility` ukrywa LOD, gdy powiązany model
  bliski ma obiekt RenderWare i pełną widoczność (`m_nVisibility == 255`).
- Po ukryciu LOD luka geometrii modelu bliskiego staje się widoczna jako
  „brakująca tekstura”.

Przykładowe potwierdzone pary:

| Near | LOD | Nazwa near | Nazwa LOD |
| --- | --- | --- | --- |
| 4246 | 4260 | `nb_road02` | `LODroad02` |
| 4248 | 4262 | `nb_road04` | `LODroad04` |
| 4254 | 4268 | `nb_road10` | `LODroad10` |
| 2060 | 2031 | `road_downtown15` | `LODd_downtown15` |

## Zakres pierwszej wersji

1. Obsługa tylko GTA Vice City 1.0 US (`gta-vc.exe`).
2. Działanie wyłącznie dla modeli typu `CSimpleModelInfo` oznaczonych jako
   drogi.
3. Zachowanie standardowego streamingu, tworzenia `RwObject` i tekstur.
4. Pozostawienie LOD jako wizualnego fallbacku, gdy gra normalnie go ukrywa.
5. Konfiguracja INI z możliwością pełnego wyłączenia moda.

Poza zakresem v1:

- globalne wyłączenie cullingu LOD;
- modyfikowanie DFF, IPL lub IDE;
- ingerencja w `dx8to12`;
- obsługa innych wersji EXE bez osobnych sygnatur.

## Architektura

Projekt: osobny DLL/ASI, np. `VcRoadLodFallback.asi`.

Proponowana struktura:

```text
VcRoadLodFallback/
  CMakeLists.txt
  src/
    dllmain.cpp
    config.cpp
    config.h
    game_addresses.cpp
    game_addresses.h
    renderer_hook.cpp
    renderer_hook.h
    model_info.h
    diagnostics.cpp
  VcRoadLodFallback.ini
  README.md
```

Zależności:

- lekka biblioteka hooków (np. MinHook) albo własny 5-bajtowy detour x86;
- opcjonalnie Plugin-SDK VC wyłącznie jako dokumentacja struktur/adresów;
- brak zależności od DX8/DX11/DX12.

## Punkt hooka

Należy namierzyć retailową implementację:

```cpp
int CRenderer::SetupBigBuildingVisibility(CEntity* entity);
```

Istotna gałąź logiczna w pseudokodzie:

```cpp
if (distance < lodNearDistance && distance < LOD_DISTANCE) {
    if (nonLod == nullptr ||
        (nonLod->GetRwObject() != nullptr && nonLod->m_nVisibility == 255)) {
        return VIS_INVISIBLE;
    }
}
```

Hook ma wykonać oryginalną logikę dla wszystkich obiektów poza drogami. Dla
rozpoznanej pary droga-LOD powinien pominąć wyłącznie powyższe ukrycie LOD.

## Zasada decyzji moda

1. Odczytać `CSimpleModelInfo` LOD bieżącej encji.
2. Odczytać jego powiązany model bliski (`m_pLodModelInfo` / related model).
3. Sprawdzić, że oba modele są prostymi modelami dróg.
4. Jeśli standardowa gałąź chciałaby ukryć LOD wyłącznie z powodu pełnej
   widoczności near modelu, zwrócić ścieżkę zachowującą renderowanie LOD.
5. W przeciwnym przypadku przekazać sterowanie bez zmian do kodu gry.

Początkowo można ograniczyć działanie do listy potwierdzonych ID modeli.
Następnie rozszerzyć je na flagę `bIsRoad`, po sprawdzeniu, że nie powoduje
artefaktów na innych road meshach.

## Minimalizacja artefaktów

Samo rysowanie near i LOD równocześnie może powodować z-fighting. Kolejność
wdrożenia:

1. Wersja diagnostyczna: zachowaj pełny LOD dla potwierdzonych par i oceniaj
   obraz oraz koszt.
2. Jeśli wystąpi z-fighting, zachowuj LOD tylko w fazie fade lub wymuszaj dla
   niego niższą alfę przez istniejącą ścieżkę fadingu RenderWare.
3. Jeśli nadal będzie widoczny konflikt, twórz whitelistę konkretnych modeli
   z luką, zamiast obejmować wszystkie drogi.

Nie należy globalnie wymuszać LOD dla budynków: prowadziłoby to do nakładania
geometrii, błędów okluzji i niepotrzebnego kosztu renderowania.

## Plan implementacji

### Etap 1 — identyfikacja binarna

1. W Ghidrze znaleźć `SetupBigBuildingVisibility` przez odwołania do listy
   dużych budynków oraz znanego kodu `ConstructRenderList`.
2. Udokumentować adres funkcji, prolog i offset gałęzi ukrywającej LOD.
3. Zdefiniować sygnaturę bajtową z maską; nie opierać instalacji hooka tylko
   na stałym adresie.
4. Zweryfikować sygnaturę na używanym `gta-vc.exe` przed zapisem pamięci.

### Etap 2 — szkielet ASI

1. Utworzyć projekt x86 DLL z eksportem/entry pointem wymaganym przez loader
   ASI używany w tej instalacji.
2. Dodać bezpieczną inicjalizację po załadowaniu `gta-vc.exe`.
3. Dodać INI:

```ini
[RoadLodFallback]
Enabled=1
Mode=Whitelist
Log=0
```

4. Przy niezgodnej sygnaturze nie instalować hooka; mod ma pozostać bierny.

### Etap 3 — hook i whitelist

1. Zaimplementować detour funkcji widoczności.
2. Przekazać wszystkie nie-road encje do oryginalnej funkcji bez zmian.
3. Dla whitelisty potwierdzonych LOD model IDs przechwycić wyłącznie decyzję
   `VIS_INVISIBLE` wynikającą z pełnej widoczności near modelu.
4. Dodać limitowany log diagnostyczny: model LOD, near model, dystans i
   podjęta decyzja.

### Etap 4 — poprawna faza przejściowa

1. Ocenić obraz po zachowaniu pełnego LOD.
2. Jeżeli z-fighting jest zauważalny, użyć istniejącej ścieżki fadingu zamiast
   renderowania obu modeli w pełnej nieprzezroczystości.
3. Zachować kolejność renderowania i stan RenderWare zarządzane przez grę;
   mod nie powinien samodzielnie wywoływać draw-callów D3D.

### Etap 5 — rozszerzenie i testy

1. Testować przejścia przy 30, 60, 144 oraz 300 FPS.
2. Testować obszary `nbeach`, `downtown` i inne mapy z road LOD.
3. Testować z `dx8to12`, działającym portem D3D11 oraz oryginalnym D3D8.
4. Sprawdzić brak regresji: budynki, tunele, okluzja, deszcz, noc i szybki
   przejazd pojazdem.
5. Dopiero po testach rozważyć `Mode=AllRoads`.

## Kryteria akceptacji

- Brak dziur drogi w potwierdzonych punktach przy 200–300 FPS.
- Brak istotnego z-fightingu lub podwójnych konturów.
- Brak wzrostu użycia pamięci w długiej sesji.
- Brak zmian w zachowaniu nie-road modeli.
- Mod działa po usunięciu `dx8to12` i nie wymaga jego API.

## Ryzyka

- Adresy dotyczą konkretnej wersji EXE; dlatego wymagane są sygnatury i
  bezpieczny fail-closed.
- Nie wszystkie road LOD-y muszą mieć tę samą relację modelową.
- Limit Adjuster może relokować część struktur; mod powinien odczytywać
  wskaźniki z kodu gry lub przez zweryfikowane sygnatury.
- Zbyt szerokie utrzymywanie LOD może pogorszyć jakość obrazu i wydajność.

## Następna konkretna czynność

W Ghidrze zidentyfikować adres i pseudokod retailowego
`CRenderer::SetupBigBuildingVisibility`, następnie stworzyć minimalny ASI z
hookiem w trybie whitelisty dla modeli `2031`, `4260`, `4262` i `4268`.
