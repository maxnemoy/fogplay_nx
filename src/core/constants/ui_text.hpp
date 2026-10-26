#pragma once

namespace fog::constants::ui {
namespace Common {
inline constexpr char BrandCaption[] = "FOG PLAY NX";
inline constexpr char Ready[] = "Готово";
inline constexpr char RetryHintSuffix[] = "   R Повторить";
inline constexpr char RublesPerHourSuffix[] = " руб./час";
inline constexpr char Connect[] = "Подключиться";
inline constexpr char Back[] = "Назад";
inline constexpr char MinutesSuffix[] = " мин.";
inline constexpr char RublesSuffix[] = " руб.";
inline constexpr char Open[] = "Открыть";
inline constexpr char Enabled[] = "Включена";
inline constexpr char Disabled[] = "Выключена";
inline constexpr char Continue[] = "Продолжить";
inline constexpr char Check[] = "Проверить";
inline constexpr char Loading[] = "Загрузка…";
inline constexpr char OfSeparator[] = " из ";
inline constexpr char MillisecondsUnit[] = "мс";
inline constexpr char MegabitsSuffix[] = " Мбит/с";
inline constexpr char MillisecondsSuffix[] = " мс";
inline constexpr char Cancel[] = "Отмена";
inline constexpr char Finish[] = "Завершить";
}

namespace Startup {
inline constexpr char PreparingLogs[] = "Подготавливаем журналы…";
inline constexpr char CheckingStorage[] = "Проверяем SD-карту…";
inline constexpr char PreparingInterface[] = "Готовим интерфейс…";
inline constexpr char StartingApplication[] = "Запускаем приложение…";
inline constexpr char ConnectingServices[] = "Подключаем службы…";
inline constexpr char WaitingForCleanup[] = "Waiting for network cleanup. Please wait.";
inline constexpr char FailureFormat[] = "%s startup failed\n\n%s\n\nUse full application mode (hold R when launching a game).\nCheck SD card and firmware date/time.\nPress + to exit.\n";
}

namespace Profile {
inline constexpr char SessionHistory[] = "История сессий";
inline constexpr char SignedOut[] = "Вы вышли из аккаунта. Сохранённый токен удалён.";
inline constexpr char AuthSaved[] = "Аккаунт подключён. Вход сохранён на SD.";
inline constexpr char Connected[] = "Аккаунт подключён";
inline constexpr char SignInAccount[] = "Войти в аккаунт";
inline constexpr char HistoryTitle[] = "ИСТОРИЯ СЕССИЙ";
inline constexpr char OwnAccount[] = "Свой аккаунт";
inline constexpr char ServerAccount[] = "Аккаунт сервера";
inline constexpr char OwnSteamSentence[] = "Вход в свой Steam.";
inline constexpr char ServerAccountSentence[] = "Аккаунт сервера.";
inline constexpr char NoSessions[] = "Сессий пока нет";
inline constexpr char HistoryExplanation[] = "Здесь появится история ваших игровых сессий.";
inline constexpr char ActiveSessions[] = "Активные сессии";
inline constexpr char ActiveSessionsDescription[] = "Подключиться к уже работающему ПК или завершить аренду";
inline constexpr char SignOut[] = "Выйти из аккаунта";
inline constexpr char SignOutDescription[] = "Удалить сохранённый вход с SD-карты";
inline constexpr char NoActiveSessions[] = "Активных сессий нет";
inline constexpr char StartRentalHint[] = "Выберите игру и сервер в каталоге, чтобы начать новую аренду.";
inline constexpr char MtsSignInTitle[] = "Вход в МТС Fog Play";
inline constexpr char MtsSignInConsent[] = "A — согласиться с условиями сервиса fogplay.mts.ru и запросить SMS. Подписка на рассылку не включается. Токен сохранится на SD для следующего запуска. SMS-код не сохраняется.";
inline constexpr char SignIn[] = "Войти";
inline constexpr char SignInTitle[] = "Вход в аккаунт";
inline constexpr char CaptchaHint[] = "Запомните текст картинки и нажмите A для ввода.";
inline constexpr char SmsHint[] = "Нажмите A и введите код из SMS.";
inline constexpr char EnterCode[] = "Ввести код";
inline constexpr char AuthNotSaved[] = "Вход не сохранён на SD. После закрытия понадобится войти снова.";
inline constexpr char HistoryMenu[] = "Журнал сессий";
inline constexpr char OwnSteamAccount[] = "Свой Steam аккаунт";
inline constexpr char EmptyHistory[] = "История сессий пуста";
inline constexpr char CompletedHistoryExplanation[] = "Здесь появятся завершённые сессии вашего аккаунта.";
inline constexpr char StartTime[] = "Начало";
inline constexpr char EndTime[] = "Окончание";
inline constexpr char ValidatingSavedAuth[] = "Проверяем сохранённый вход…";
inline constexpr char ConnectingService[] = "Подключение к Fog Play…";
inline constexpr char RestoreFailedHint[] = "Не удалось восстановить вход. A — войти заново.";
inline constexpr char ValidationFailedHint[] = "Сохранённый вход не прошёл проверку. A — войти заново.";
inline constexpr char PreviousRentalSignInHint[] = "Нужно проверить прежнюю аренду. Y — войти в аккаунт.";
inline constexpr char AuthSaveFailed[] = "Вход выполнен, но сохранить его на SD не удалось.";
inline constexpr char SignInIncompleteHint[] = "Вход не завершён. B — назад, Y — новая попытка.";
}

namespace Settings {
inline constexpr char LayoutSaved[] = "Раскладка сохранена на SD.";
inline constexpr char ImageTitle[] = "ИЗОБРАЖЕНИЕ";
inline constexpr char ConnectionTitle[] = "ПОДКЛЮЧЕНИЕ";
inline constexpr char Title[] = "НАСТРОЙКИ";
inline constexpr char ConnectionHints[] = "← / → или A Изменить режим   B Настройки";
inline constexpr char ImageHints[] = "↑ / ↓ Параметр   ← / → или A Изменить   B Настройки";
inline constexpr char Image[] = "Изображение";
inline constexpr char ImageDescription[] = "Кодек, качество, битрейт и ключевые кадры";
inline constexpr char XboxLayout[] = "Xbox-раскладка";
inline constexpr char XboxLayoutDescription[] = "В меню: B — выбор, A — назад. В игре — Xbox.";
inline constexpr char NintendoLayoutDescription[] = "В меню: A — выбор, B — назад. В игре — Nintendo.";
inline constexpr char Connection[] = "Подключение";
inline constexpr char ConnectionDescription[] = "Выбор пути: обычный, авто или через TURN";
inline constexpr char Menu[] = "Настройки";
inline constexpr char AutoSaveNotice[] = "Изменения сохраняются сразу на SD.";
inline constexpr char ConnectionAutoSaveNotice[] = "Изменения сохраняются на SD. Применяются при следующем подключении.";
inline constexpr char VideoLoadFailed[] = "Настройки не прочитаны. Используются стандартные; измените, чтобы сохранить.";
inline constexpr char H265FallbackPreserved[] = "H.265 недоступен в этой сборке. Выбран H.264; файл не изменён.";
inline constexpr char ConnectionLoadFailed[] = "Настройки не прочитаны. Используется обычный режим; измените, чтобы сохранить.";
inline constexpr char LayoutSaveFailed[] = "Не удалось сохранить раскладку. Настройка не изменена.";
inline constexpr char Saved[] = "Сохранено на SD.";
inline constexpr char NotSaved[] = "Не сохранено на SD. После перезапуска изменения потеряются.";
inline constexpr char RequestSentNotice[] = " Запрошено; подтверждение сервера не гарантируется.";
inline constexpr char RequestNotSentNotice[] = " Канал недоступен; для текущей сессии запрос не отправлен.";
inline constexpr char H265Fallback[] = "H.265 недоступен в этой сборке. Используется H.264.";
inline constexpr char CodecPrefix[] = "Кодек: ";
inline constexpr char QualityPrefix[] = "Качество сервера: ";
inline constexpr char BitrateLimitPrefix[] = "Запрошенный предел: ";
inline constexpr char KeyframesPrefix[] = "Ключевые кадры: ";
inline constexpr char H265ExperimentalNotice[] = "H.265 экспериментальный. Изменяется только до подключения.";
inline constexpr char H265Unavailable[] = "H.265 недоступен в этой сборке.";
inline constexpr char QualityDescription[] = "Авто / Низкое / Среднее / Высокое / Ультра — запрос серверу.";
inline constexpr char BitrateDescription[] = "5–40 Мбит/с. Сервер может использовать другой фактический битрейт.";
inline constexpr char KeyframeDescription[] = "При потере или каждые 2 / 5 / 10 с — интервал запроса, не частота видео.";
inline constexpr char ConnectionSaved[] = "Сохранено на SD. Применится при следующем подключении.";
inline constexpr char ConnectionSaveFailed[] = "Не удалось сохранить на SD. Режим не изменён.";
inline constexpr char ConnectionModePrefix[] = "Режим: ";
inline constexpr char NormalConnectionDescription[] = "Обычный: автоматический выбор пути, как в прежних версиях.";
inline constexpr char AutoConnectionDescription[] = "Авто: TURN после 2 обрывов за 2 мин, до конца текущей аренды.";
inline constexpr char ReconnectCountingDescription[] = "Повторные неудачные попытки одного восстановления не считаются новым обрывом.";
inline constexpr char TurnConnectionDescription[] = "Через TURN: подключаться через сервер ретрансляции с самого начала.";
inline constexpr char BeforeConnectionSuffix[] = " (до подключения)";
}

namespace Stream {
inline constexpr char ConnectingMenuHint[] = "Подключение. Минус + Плюс — меню трансляции.";
inline constexpr char RestoredMenuHint[] = "Связь восстановлена. Минус + Плюс — меню трансляции.";
inline constexpr char ReconnectAttemptPrefix[] = "Связь: попытка ";
inline constexpr char ReconnectAttemptSuffix[] = "/3 за 2 мин. Та же аренда; оплата продолжается.";
inline constexpr char ReconnectLimitHint[] = "Минус + Плюс — меню и завершение аренды. Лимит восстановления — 90 секунд.";
inline constexpr char RecoveringVideo[] = "Восстанавливаем видео… Минус + Плюс — меню сессии";
inline constexpr char OverlayMenuHint[] = "Минус + Плюс — меню: мышь, клавиатура, изображение, завершение.";
inline constexpr char Reconnecting[] = "Восстанавливаем связь…";
inline constexpr char ReconnectNotice[] = "Повторное подключение к той же аренде. Оплата продолжается. Минус + Плюс — меню завершения.";
inline constexpr char ConnectingComputer[] = "Подключаемся к ПК…";
inline constexpr char FirstFrameHint[] = "Ожидаем первый видеокадр. Минус + Плюс — меню трансляции.";
inline constexpr char ReconnectStatus[] = "Связь потеряна. Повторяем подключение к той же аренде; оплата продолжается.";
inline constexpr char KeyframeRequested[] = "Ключевой кадр запрошен; ответ сервера не гарантируется.";
inline constexpr char KeyframeRequestFailed[] = "Не удалось запросить кадр: видеоканал недоступен.";
inline constexpr char TextSent[] = "Текст отправлен на ПК.";
inline constexpr char TextSendFailed[] = "Не удалось передать текст: канал управления недоступен.";
inline constexpr char KeySent[] = "Клавиша передана на ПК.";
inline constexpr char ControlUnavailable[] = "Канал управления недоступен.";
inline constexpr char Space[] = "Пробел";
inline constexpr char Delete[] = "Удалить";
inline constexpr char Clear[] = "Очистить";
inline constexpr char SendText[] = "Отправить текст";
inline constexpr char EnterKey[] = "Enter ПК";
inline constexpr char TabKey[] = "Tab ПК";
inline constexpr char BackspaceKey[] = "Backspace ПК";
inline constexpr char EscapeKey[] = "Escape ПК";
inline constexpr char MouseControlsHint[] = "Мышь: левый стик — курсор · A / ZR — левая кнопка · X — правая";
inline constexpr char MouseTouchHint[] = "L / R — прокрутка · касание — курсор и перетаскивание · B — меню";
inline constexpr char SessionControlsTitle[] = "Управление сессией";
inline constexpr char Mouse[] = "Мышь";
inline constexpr char Keyboard[] = "Клавиатура";
inline constexpr char FinishSession[] = "Завершить сессию";
inline constexpr char CloseMenu[] = "Закрыть меню";
inline constexpr char MenuControlsHint[] = "A — выбрать · B — закрыть";
inline constexpr char ImageSettingsTitle[] = "Изображение · трансляция продолжается";
inline constexpr char ImageSettingsHint[] = "Кодек меняется до подключения. ← / → или A — изменить · B — меню";
inline constexpr char RequestKeyframe[] = "Запросить ключевой кадр сейчас";
inline constexpr char BitrateNotice[] = "Битрейт может отличаться. Запрос кадров не задаёт частоту видео.";
inline constexpr char FinishSessionTitle[] = "Завершить сессию?";
inline constexpr char FinishSessionNotice[] = "Аренда завершится после подтверждения сервера.\nДождитесь результата.";
inline constexpr char ContinueSession[] = "Продолжить сессию";
inline constexpr char KeyboardHint[] = "Текст отправляется только кнопкой «Отправить текст». B — отмена";
inline constexpr char EnterTextPlaceholder[] = "Введите текст…";
inline constexpr char TextByteCountSuffix[] = " / 512 байт · ";
inline constexpr char LocalEditingHint[] = "Удалить / Очистить — местный текст. Кнопки «ПК» сразу отправляют клавишу.";
}

namespace Catalog {
inline constexpr char Title[] = "ВЫБЕРИ ИГРУ";
inline constexpr char Hints[] = "A Выбрать   B Меню   X Поиск   Y Аккаунт   ↑ Жанры / прокрутка";
inline constexpr char GenresTitle[] = "ЖАНРЫ";
inline constexpr char GenreHints[] = "↑ / ↓ / ← / → Жанр   A Применить   B Назад";
inline constexpr char AllGenres[] = "Все жанры";
inline constexpr char LoadingCatalog[] = "Загружаем каталог…";
inline constexpr char LoadFailedRetryHint[] = "Не удалось загрузить каталог. R — повторить • X — поиск";
inline constexpr char LoadingGames[] = "Загружаем игры";
inline constexpr char LoadingExplanation[] = "Каталог появится после ответа сервера. X — изменить поиск.";
inline constexpr char LoadFailed[] = "Не удалось загрузить игры";
inline constexpr char AllGames[] = "Все игры";
inline constexpr char LoadFailedPrefix[] = "Не удалось загрузить игры. R — повторить: ";
inline constexpr char LoadingStatus[] = "Загрузка игр…";
inline constexpr char LoadMoreStatus[] = "Подгружаем игры…";
inline constexpr char Menu[] = "Игры";
inline constexpr char GenreDialogHints[] = "A Выбрать    B Назад    ↑ / ↓ Прокрутка";
inline constexpr char RecentGames[] = "Недавно играли";
inline constexpr char RatingPrefix[] = "Рейтинг: ";
inline constexpr char DefaultHints[] = "A Выбрать   B Назад   X Поиск   Y Аккаунт   L / R Страницы";
}

namespace PlayStation {
inline constexpr char RefreshingServers[] = "Обновление серверов…";
inline constexpr char SourceStatus[] = "PS5 · MTS FogPlay  |  PC · FogHub";
inline constexpr char CatalogHints[] = "A Подключиться   B Назад   X Обновить   Y Аккаунт";
inline constexpr char Online[] = "Онлайн";
inline constexpr char Offline[] = "Офлайн";
inline constexpr char UnknownStatus[] = "Статус неизвестен";
inline constexpr char Loading[] = "Загружаем PlayStation";
inline constexpr char NotLoaded[] = "Список не загружен";
inline constexpr char RefreshHint[] = "X — обновить список с сайта Fog Play.";
inline constexpr char ConnectionNotice[] = " — по списку МТС Fog Play. Подключение проверится при заказе.\n"
    "Игра выбирается на PS5. Свой Steam не используется.\n"
    "Начнётся платная аренда. При выходе приложение запросит её завершение.";
inline constexpr char TariffUnconfirmedHint[] = "Тариф PlayStation не подтверждён. Вернитесь к списку и нажмите X.";
inline constexpr char CheckingTariff[] = "Проверяем актуальность тарифа PlayStation…";
}

namespace Search {
inline constexpr char TitlePrefix[] = "ПОИСК: ";
inline constexpr char NoGamesSearchHint[] = "Игры не найдены. X — поиск";
inline constexpr char RetrySearchHint[] = "R — повторить • X — изменить поиск";
inline constexpr char NoGames[] = "Игры не найдены";
inline constexpr char ChangeQueryHint[] = "X — изменить поиск";
inline constexpr char Menu[] = "Поиск";
inline constexpr char NoGamesYet[] = "Игры пока не найдены";
inline constexpr char ChangeOrRefreshHint[] = "Измените поиск или обновите каталог.";
}

namespace Server {
inline constexpr char CatalogHints[] = "A Подключиться   B Назад   X Обновить   L / R Страницы   ≈ оценка через МТС";
inline constexpr char ConnectHints[] = "A Подключиться   B Назад";
inline constexpr char RefreshPingHints[] = "A Подключиться   B Назад   X Обновить пинг";
inline constexpr char PendingSessionHints[] = "A Проверить / выбрать   B Завершить аренду   Y Профиль   Минус + Плюс: меню в игре";
inline constexpr char BestPrice[] = "Лучшая цена";
inline constexpr char Balanced[] = "Оптимальный";
inline constexpr char Powerful[] = "Мощный";
inline constexpr char None[] = "Нет серверов";
inline constexpr char All[] = "Все серверы";
inline constexpr char NoSuitableServers[] = "Нет подходящих серверов";
inline constexpr char RefreshPagesHints[] = "X — обновить • L / R — страницы";
inline constexpr char ConnectPrefix[] = "Подключиться к ";
inline constexpr char HourlyPriceSentenceSuffix[] = " руб./час. ";
inline constexpr char PingEstimateSuffix[] = " — оценка через узлы МТС.";
inline constexpr char PaidRentalNotice[] = "\nНачнётся платная аренда. При выходе приложение запросит её завершение.";
inline constexpr char AdoptRentalTitle[] = "Подключиться к действующей аренде?";
inline constexpr char AdoptRentalNotice[] = "Приложение будет завершать эту сессию при выходе, уходе в HOME или потере видео. Минус + Плюс — меню завершения.";
inline constexpr char ConnectingTitle[] = "Подключение к серверу";
inline constexpr char ManualCheckHint[] = "\nЕсли связь потеряна, проверьте аренду на fogplay.mts.ru с телефона. X — подтвердить ручную проверку.";
inline constexpr char CheckButtonHint[] = "Проверить (A)";
inline constexpr char FinishButtonHint[] = "Завершить (B)";
inline constexpr char ManualResolutionTitle[] = "Вы проверили заказ на сайте?";
inline constexpr char ManualResolutionNotice[] = "Подтверждайте только если в официальном аккаунте нет работающей аренды и ожидающего оплаты заказа. Это снимает локальную блокировку; команду отмены на сервер не отправляет.";
inline constexpr char ManualResolutionConfirmed[] = "Я проверил, аренды нет";
inline constexpr char FinishGameSessionTitle[] = "Завершить игровую сессию?";
inline constexpr char FinishRentalNotice[] = "Аренда завершится после подтверждения сервера. Дождитесь результата.";
inline constexpr char FinishRental[] = "Завершить аренду";
inline constexpr char TerminationUnconfirmed[] = "Завершение аренды не подтверждено";
inline constexpr char TerminationRecoveryHint[] = "A — проверить и завершить. Y — войти в прежний аккаунт. При отсутствии связи завершите сессию на fogplay.mts.ru с телефона: списания могут продолжаться.";
inline constexpr char NoComputers[] = "Нет доступных компьютеров";
inline constexpr char RefreshOrChangeGameHint[] = "Обновите список или выберите другую игру.";
inline constexpr char PingNotMeasured[] = "Пинг не измерен";
inline constexpr char Unavailable[] = "Недоступен";
inline constexpr char AutoSelection[] = "Автоподбор сервера";
inline constexpr char NotAvailable[] = "Не доступно";
inline constexpr char DataNotLoaded[] = "Данные пока не загружены";
inline constexpr char NoSuitableComputers[] = "Нет подходящих ПК";
inline constexpr char WaitingReady[] = "Ожидаем готовность сервера…";
inline constexpr char ConnectionCancelled[] = "Подключение отменено. Заказ не создавался.";
inline constexpr char ConnectionNotConfirmed[] = "Подключение не подтверждено. Обновите список и войдите в аккаунт.";
inline constexpr char CreatingOrder[] = "Создаём заказ. Ожидаем ответ Fog Play…";
inline constexpr char TariffChanged[] = "Тариф или цена изменились. Проверьте список и подтвердите подключение заново.";
inline constexpr char LocalBlockCleared[] = "Локальная отметка снята после вашей проверки аккаунта.";
inline constexpr char TerminationConfirmed[] = "Сервер подтвердил завершение сессии.";
inline constexpr char TerminationRetrySignInHint[] = "Остановка НЕ подтверждена. Повторяем. Y — повторный вход.";
inline constexpr char PreviousRentalNotMatched[] = "Прежняя сессия не сопоставлена. Проверьте её на сайте Fog Play.";
inline constexpr char TariffUnconfirmedRefreshHint[] = "Тариф не подтверждён на сайте. Обновите список кнопкой X.";
inline constexpr char NumberPrefix[] = "Сервер №";
inline constexpr char NoCategoryOffer[] = "Нет предложения в этой категории. Выберите все серверы.";
inline constexpr char TariffUnconfirmed[] = "Тариф не подтверждён";
inline constexpr char MeasuringPing[] = "Пинг: измеряем…";
inline constexpr char PingUnknown[] = "Пинг: нет данных";
inline constexpr char StopRetryNotice[] = "Повторяем остановку. Проверьте сессию на fogplay.mts.ru с телефона.";
inline constexpr char ManualExitHint[] = "Сначала завершите аренду на сайте. Затем удерживайте − и + 3 секунды для выхода.";
inline constexpr char ManualExitWarning[] = "Этот выход не подтверждает остановку: журнал восстановления останется на SD.";
inline constexpr char StopAfterResponse[] = "Завершение запрошено. Ждём текущий ответ сервера, затем остановим аренду.";
inline constexpr char OrderRejected[] = "Сервер отклонил заказ. Обновите список ПК.";
inline constexpr char RentalFinished[] = "Аренда завершена.";
inline constexpr char WaitingStopData[] = "Ожидаем данные для остановки. Заказ пока не отменён.";
inline constexpr char OrderRecoveryHint[] = "Заказ требует проверки. A — проверить, B — остановить, Y — вход.";
inline constexpr char AutoCheckPausedHint[] = "Автопроверка приостановлена. A — проверить ещё раз, B — остановить аренду.";
inline constexpr char ReadyObservationsPrefix[] = "Проверяем готовность сессии (";
inline constexpr char PaymentConfirmedWaitingAddress[] = "Оплата подтверждена. Ожидаем адрес сессии…";
inline constexpr char PreparingConnection[] = "Сервер готовится к подключению…";
inline constexpr char WaitingAddress[] = "Ожидаем адрес сессии…";
inline constexpr char OrderCreated[] = "Заказ создан. Ожидаем сервер…";
inline constexpr char PaymentRequired[] = "Сервис запросил оплату. Проверьте заказ на fogplay.mts.ru.";
inline constexpr char OrderRecorded[] = "Заказ записан. Ожидаем готовность сервера…";
inline constexpr char RefreshingTariff[] = "Список устарел. Обновляем тариф: подтвердите новую цену перед подключением.";
inline constexpr char RequestingStop[] = "Запрашиваем завершение аренды…";
inline constexpr char Stopping[] = "Завершаем аренду на сервере. Ждём подтверждение.";
}

namespace Settings {
inline constexpr char H265Codec[] = "H.265 / HEVC · экспериментально";
inline constexpr char H264Codec[] = "H.264";
inline constexpr char AutoQuality[] = "Авто";
inline constexpr char LowQuality[] = "Низкое";
inline constexpr char MediumQuality[] = "Среднее";
inline constexpr char HighQuality[] = "Высокое";
inline constexpr char UltraQuality[] = "Ультра";
inline constexpr char KeyframeIntervalPrefix[] = "Запрос каждые ";
inline constexpr char KeyframeIntervalSuffix[] = " с";
inline constexpr char KeyframesOnLoss[] = "Только при потере (рекомендуется)";
inline constexpr char RelayMode[] = "Через TURN";
inline constexpr char NormalMode[] = "Обычный";
}
namespace Catalog {
inline constexpr char Shooter[] = "Шутер";
inline constexpr char Racing[] = "Гонки";
inline constexpr char Horror[] = "Хоррор";
inline constexpr char Fantasy[] = "Фэнтези";
inline constexpr char Action[] = "Экшен";
inline constexpr char Multiplayer[] = "Мультиплеер";
inline constexpr char Indie[] = "Инди";
inline constexpr char Strategy[] = "Стратегия";
inline constexpr char Rpg[] = "RPG";
inline constexpr char Anime[] = "Аниме";
inline constexpr char Simulation[] = "Симулятор";
inline constexpr char Casual[] = "Казуальные";
inline constexpr char Sport[] = "Спорт";
inline constexpr char Arcade[] = "Аркады";
inline constexpr char Survival[] = "Выживание";
inline constexpr char Platformer[] = "Платформер";
inline constexpr char Fighting[] = "Файтинги";
inline constexpr char Mmorpg[] = "MMORPG";
}

}
