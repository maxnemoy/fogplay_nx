# Сторонние компоненты

Лицензия собственного кода клиента — GPL-2.0-or-later, см. [LICENSE](../LICENSE). Она не заменяет перечисленные ниже лицензии зависимостей.

Указание автора `maxnemoy` относится к клиенту FogPlay NX. Авторские уведомления зависимостей не изменены. Копии основных лицензионных текстов сохранены в `licenses/`; полные уведомления отдельных исходных файлов остаются в upstream-деревьях.

| Компонент | Источник / лицензия |
| --- | --- |
| libdatachannel v0.24.1 | [upstream](https://github.com/paullouisageneau/libdatachannel), MPL-2.0 |
| libjuice | [upstream](https://github.com/paullouisageneau/libjuice), MPL-2.0 |
| usrsctp | [upstream](https://github.com/sctplab/usrsctp), BSD |
| libsrtp | [upstream](https://github.com/cisco/libsrtp), BSD |
| Mbed TLS 3.6.4 | [upstream](https://github.com/Mbed-TLS/mbedtls), Apache-2.0 OR GPL-2.0-or-later; отдельные файлы Everest имеют Apache-2.0, см. `licenses/everest-NOTICES.txt` |
| nlohmann/json | [upstream](https://github.com/nlohmann/json), MIT; встроен в `third_party/nlohmann/` |
| FFmpeg 7.1 / NVTEGRA | [FFmpeg](https://ffmpeg.org/), [devkitPro portlibs](https://github.com/devkitPro/portlibs); используемые NVTEGRA-файлы GPL-2.0-or-later |
| SDL2, SDL2_image, SDL2_ttf | [libsdl-org](https://github.com/libsdl-org), zlib; FreeType, HarfBuzz и декодеры изображений имеют собственные уведомления |
| libnx | [switchbrew/libnx](https://github.com/switchbrew/libnx), ISC |
| curl | [curl/curl](https://github.com/curl/curl), curl license |
| Opus | [xiph/opus](https://github.com/xiph/opus), BSD |
| libpng, libjpeg-turbo, libwebp, FreeType, HarfBuzz, zlib, bzip2 | Статические зависимости Switch portlibs; сохраняются соответствующие upstream-лицензии |
| Корневые сертификаты | Mozilla CA bundle от [curl.se](https://curl.se/docs/caextract.html), заголовок происхождения сохранён в `romfs/cacert.pem` |

## Воспроизводимые источники и изменения

`tools/rtc/fetch.sh` получает libdatachannel commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48` и Mbed TLS commit `c765c831e5c2a0971410692f92f7a81d6ec65ec2` вместе с закреплёнными submodules. `tools/rtc/patch-sources.py` и C-адаптеры применяют изменения для Switch. Файлы `.patch` сохранены как справочные снимки отдельных изменений и сборкой не применяются. Копии fetched-репозиториев и статических библиотек не входят в публикуемые исходники.

`tools/ffmpeg/build-diag-overlay.py` проверяет SHA-256 FFmpeg 7.1 и двух devkitPro-патчей, применяет `nvdec-diagnostics.patch`, собирает только диагностический NVTEGRA-объект и проверяет неизменность остальных объектов установленного `libavcodec.a`. Точная конфигурация и контрольные суммы записываются в `third_party/ffmpeg-diag/switch/lib/manifest.json`. Исходные патчи и GPL-текст остаются в `tools/ffmpeg`.

Для распространения собранного NRO нужны условия всех включённых компонентов, в том числе соответствующие исходники/изменения/конфигурация GPL-компонентов. Архив приложения содержит доступные лицензионные тексты и манифест декодера; он сам по себе не является архивом всех исходников статических библиотек. Публикация исходного клиента и распространение бинарной сборки — отдельные действия.

Обложки игр и фирменные обозначения не становятся собственностью автора клиента и не перелицензируются вместе с кодом.

Для публичного релиза NRO подготовьте соответствующие исходники включённых GPL/MPL-компонентов, применённые изменения и сценарии сборки вместе с исходниками клиента. Одних ссылок на upstream или текста «исходники доступны по запросу» недостаточно. Текущий архив клиента не заменяет этот комплект. У компонентов Apache-2.0 отдельно проверяется совместимость выбранной версии GPL; собственный код допускает GPL-2.0 **или более позднюю** версию.
