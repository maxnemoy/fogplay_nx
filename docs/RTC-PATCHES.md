# Проверяемая серия RTC-патчей

Активный рецепт — `tools/rtc/patches/series.json` и три патча, перечисленные в нём. `tools/rtc/patch-sources.py` до изменения живого дерева проверяет:

- точные HEAD libdatachannel `a02b751917ac8afc8c58dc6f4461d25ff9465d48`, libjuice `5948a4162d37bc213d6051b67ee2876ccc5a99a6` и usrsctp `fec583d54493f879d2ae44a743423bf8a04371ab`;
- SHA-256 патчей, входных заголовков и каждого pristine/patched файла;
- отсутствие неизвестных локальных изменений и смешанного/частичного состояния;
- применение всей серии к pristine-объектам из Git во временном каталоге и точные итоговые хеши.

Живые файлы обновляются только после успешной полной проверки во временном каталоге. Symbolic links в declared paths и их родительских каталогах отвергаются до записи, включая dangling links. Каждый созданный temporary/backup path регистрируется до write/close/chmod и удаляется при pre-publication failure. При публикации сохраняются original-byte backups; обычная ошибка rename откатывает уже заменённые файлы и позволяет штатно повторить вызов. Если дерево уже полностью соответствует серии, вызов является проверенным no-op. Checker создаёт isolated Git index/config с `core.worktree` во временном архиве и read-only object alternates, не наследуя живой nested worktree. `manifest.py` включает JSON, патчи и остальные RTC-рецепты в входы кэша.

Старые `tools/rtc/patch-*.py`, `libjuice-switch.patch` и `usrsctp-switch.patch` сохранены как происхождение изменений. Они больше не запускаются активным рецептом; исторические патчи сами по себе не описывают полный результат.

## Происхождение нестандартного API и проверки

Все перечисленные ниже API являются локальными расширениями FogPlay, а не обещанием совместимости с другими версиями upstream. Их содержимое получено из прежнего активного Python-рецепта и сопоставлено с read-only Git baseline закреплённых ревизий.

| Изменение / API | Происхождение | Compatibility-тесты |
| --- | --- | --- |
| `juice_get_diagnostics`, `juice_diagnostics_t`, причины отказов ICE; бюджет UDP receive | `patch-ice-diagnostics.py` | `udp-budget-test.c`, `ice-diagnostics-test.cpp` |
| `PeerConnection::iceDiagnostics()` | `patch-ice-diagnostics.py` | `ice-diagnostics-test.cpp`, host loopback |
| `DtlsDiagnostics`, `PeerConnection::getDtlsDiagnostics()`, метрики очереди и receive | `patch-dtls-diagnostics.py` | `dtls-queue-test.cpp`, `dtls-diagnostics-test.cpp` |
| mbedTLS receive isolation, fairness, SSL lock metrics | `patch-dtls-receive.py` | `dtls-receive-test.cpp`, `dtls-diagnostics-test.cpp` |
| `DtlsIngressQueue`, admission и per-stage receive metrics | `patch-dtls-ingress.py`, `dtls-ingress-queue.h` | `dtls-ingress-test.cpp`, `dtls-receive-test.cpp` |
| Deferred RR/REMB, `setDeferredReports`, `flushPendingReports`, `discardPendingReports`, feedback diagnostics | `patch-rtcp-feedback.py` | `rtcp-feedback-test.cpp` |
| RTP/RTCP callback и SRTP send timing fields | `patch-media-send-diagnostics.py` | `media-send-diagnostics-test.cpp` |
| Слабый `fogObserveThread` и `fog_observe_thread_start` | `patch-thread-observation.py`, `thread-observation.h` | `thread-observation-test.cpp` |
| `rtc::JoinCleanupThreads()` и owned cleanup threads | Блоки cleanup в прежнем `patch-sources.py` | `cleanup-thread-test.cpp`, `init-rollback-test.cpp`, `loopback-failure-test.cpp`; режим `RTC_TEST_JOINABLE_CLEANUP` |
| Switch RNG, socket wakeup, `sockaddr_conn`, rollback `PollService`/`Init`, отказ hostname candidates, mbedTLS idle markers | Прежний `patch-sources.py`; ранние RNG-изменения также отражены в исторических `libjuice-switch.patch`/`usrsctp-switch.patch` | `wakeup-check-test.cpp`, `numeric-layout-contract.c`, `numeric-contract.c`, `pipe-contract.c`, `test-tls-idle.sh`, lifecycle-тесты выше |

## Локальная проверка

```sh
python3 tools/rtc/patch-sources.py
python3 tools/rtc/check-source-patches.py
python3 tools/test/build_tools_tests.py
```

Первый вызов на уже подготовленном дереве обязан завершиться no-op. Второй архивирует только pinned Git sources и сравнивает результат с живым деревом; сетевой fetch не выполняется. Расширенные lifecycle/contract/host RTC тесты остаются отдельным gate через `tools/test/rtc.sh`; успешная проверка серии не заменяет сборку и эти тесты.
