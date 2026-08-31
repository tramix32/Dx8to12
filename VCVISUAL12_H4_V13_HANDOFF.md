# VCVisual12: integracja maski RT z protokołu v14

Dx8to12 zwraca teraz po pierwszej ukończonej paczce prawdziwy, lokalny dla
urządzenia x86 zasób `Texture2D`:

- rozmiar: pobieraj przez `Dx8to12_GetRtShadowOutputWidth/Height`
  (obecnie 320x180),
- format: pobieraj przez `Dx8to12_GetRtShadowOutputFormat`
  (obecnie `DXGI_FORMAT_R8G8B8A8_UNORM`),
- stan w callbacku renderującym: `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`,
- własność: wskaźnik jest pożyczony; nie wywołuj `Release` i nie zachowuj go
  po zmianie `Dx8to12_GetSwapChainGeneration`/resecie urządzenia.

## Najważniejsza zmiana synchronizacji

W protokole v14 `Dx8to12_GetRtShadowDoneFence()` zwraca `nullptr`, a
`Dx8to12_GetRtShadowDoneFenceValue()` zwraca `0` także wtedy, gdy tekstura
jest już poprawna. Nie oznacza to „brak wyniku”. Upload tekstury i przejście
do `PIXEL_SHADER_RESOURCE` są zapisane wcześniej na tej samej liście poleceń,
na której później wywoływany jest callback moda.

Warunek gotowości v13:

```cpp
auto* shadow = static_cast<ID3D12Resource*>(get_shadow_resource());
const UINT width = get_shadow_width();
const UINT height = get_shadow_height();
const UINT format = get_shadow_format();
const bool ready = shadow != nullptr && width != 0 && height != 0 &&
                   format == DXGI_FORMAT_R8G8B8A8_UNORM;
```

Nie wykonuj `queue->Wait()` dla pary `fence=nullptr`, `value=0`. Dla zgodności
z ewentualnym starszym/przyszłym kanałem współdzielonym wykonuj GPU wait tylko
gdy jednocześnie `fence != nullptr && value != 0`.

## Kompozycja cieni (LightingMode=2)

Utwórz SRV `Texture2D<float4>` dla pożyczonego RGBA8 i narysuj fullscreen triangle
w callbacku z `Dx8to12_RegisterRenderCallback`. Kanał R ma wartość 1 dla
oświetlonego piksela/nieba i 0 dla trafienia promienia cienia. Minimalna
kompozycja to pomnożenie koloru sceny przez np. `lerp(0.55, 1.0, mask)`.

Nie wolno bezpośrednio czytać aktualnego backbufferu równocześnie jako RTV i
SRV. Do poprawnego compositingu potrzebna jest kopia koloru sceny do własnej
tekstury moda albo blending fullscreen pass, którego shader zwraca czarny z
alfą zależną od `(1-mask)` (np. alpha `0.45*(1-mask)`). Druga opcja nie wymaga
SRV backbufferu.

Maskę skaluj bilinearnie do rozmiaru viewportu. Odtwarzaj SRV/PSO i wszystkie
zasoby zależne od backbufferu po zmianie `Dx8to12_GetSwapChainGeneration`.

## Tryby 3 i 4

Aktualny kanał v13 zawiera cienie kierunkowe. `LightingMode=3/4` nadal używa
tej maski jako bezpiecznej bazy oraz per-pixel lighting po stronie Dx8to12;
osobne kanały odbić i GI będą dodane do API oddzielnie. Nie interpretuj R8
jako kolor odbicia lub irradiance.
