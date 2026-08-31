# VCVisual12 — wynik RT protokołu v14

`Dx8to12_GetRtShadowOutputResource()` zwraca po pierwszym wyniku pożyczoną,
x86-lokalną `Texture2D` 320x180 w `DXGI_FORMAT_R8G8B8A8_UNORM`:

- R: widoczność światła kierunkowego (`LightingMode >= 2`),
- G: trafienie promienia odbicia (`LightingMode >= 3`),
- B: widoczność promienia rozproszonego / baza GI (`LightingMode >= 4`),
- A: primary ray trafił geometrię sceny.

Rozmiar i format zawsze pobieraj z eksportów Width/Height/Format. Zasobu nie
wolno `Release` ani zachowywać przez reset/zmianę generacji swap chaina.

Fence i fence value są w v14 odpowiednio `nullptr` i `0`. To poprawny gotowy
wynik: upload i przejście do `PIXEL_SHADER_RESOURCE` zostały zapisane wcześniej
na tej samej liście poleceń co callback moda. Nie wykonuj `queue->Wait()` i nie
odrzucaj tekstury tylko dlatego, że fence jest zerowy.

Dla cieni narysuj fullscreen triangle z blendingiem czerni:
`alpha = shadow_strength * (1 - rt.r) * rt.a`. Nie trzeba czytać backbufferu
jako SRV. Kanały G/B są na razie sygnałami geometrycznymi, a nie gotowym
kolorem materiału; mod może je wizualizować lub użyć jako wag dla własnego
environment/ambient pass.
