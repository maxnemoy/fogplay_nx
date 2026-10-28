#pragma once
namespace fog::rating::text {
inline constexpr char Title[] = "ОЦЕНКА СЕАНСА FOGHUB";
inline constexpr char Introduction[] = "Сеанс завершён. Как прошла игра?";
inline constexpr char Choice[] = "Выберите оценку от 1 до 5";
inline constexpr char ChoiceHint[] = "← / → — оценка · ↑ / ↓ — выбор · A — действие · B — позже";
inline constexpr char LoadingHint[] = "Дождитесь завершения проверки…";
inline constexpr char RetryHint[] = "A — проверить ещё раз · B — позже";
inline constexpr char DoneHint[] = "A / B — вернуться к серверам";
inline constexpr char Comment[] = "Комментарий (необязательно, до 200 символов)";
inline constexpr char EmptyComment[] = "A — добавить комментарий";
inline constexpr char Send[] = "Отправить оценку";
inline constexpr char Later[] = "Позже";
inline constexpr char Loading[] = "Проверяем завершённый сеанс…";
inline constexpr char Sending[] = "Отправляем оценку…";
inline constexpr char Sent[] = "Спасибо! Оценка отправлена.";
inline constexpr char AlreadyRated[] = "Этот сеанс уже оценён.";
inline constexpr char None[] = "Нет завершённых сеансов, ожидающих оценки.";
inline constexpr char LoadFailed[] = "Не удалось загрузить оценку. A — проверить ещё раз, B — позже.";
inline constexpr char SubmitFailed[] = "Отправка не подтверждена. Выбор сохранён; отправьте ещё раз для проверки.";
inline constexpr char CommentTooLong[] = "Комментарий слишком длинный. Можно ввести до 200 символов; эмодзи занимают два.";
inline constexpr char WrongSession[] = "Не удалось подтвердить завершённый сеанс. Проверьте ещё раз.";
inline constexpr char SignIn[] = "Войдите в исходный аккаунт FogHub, чтобы оценить сеанс.";
inline constexpr char Unavailable[] = "Оценка FogHub сейчас недоступна.";
inline constexpr char Back[] = "Вернуться к серверам";
inline constexpr char DeferredHint[] = "Форму можно открыть позже: Профиль → FogHub → R.";
inline constexpr char ProfileHint[] = "R — оценить завершённый сеанс";
}
