These are sanitized structural test fixtures, not HTTP captures of a paid flow.

- `login.html`, `games.html`, `computers.html`: public server HTML read on 2026-10-26 from `/site/login`, `/game/list`, `/game/preview?id=70`. Only `main` is retained; scripts are removed, CSRF is replaced by a literal fixture placeholder, and unused MTS click-play signature query values are removed. Catalog counts and card HTML remain the observed values.
- `history-active.html`, `history-terminal.html`: actual post-JavaScript table fragments from the authorized 2026-10-26 stage-1 observations. IDs/dates were pseudonymized before this work. The outer main, heading, logout marker and two-row summary are synthetic wrappers matching separately observed markers. These are not evidence that the server response equals the rendered DOM.
- `history-reason-filter-empty.html`: same stage-1 DOM source, preserving the selected reason filter. It must be rejected as an unfiltered history response. Tests derive an explicit empty unfiltered case from this structure.
- `account.html`: minimal synthetic fixture of the live-verified `Информация`, `table.detail-view`, `Логин`, logout form structure. Neither real usernames nor API tokens were captured. Account identity is the exact trimmed login prefixed with `login:`; this is not a proven immutable backend account ID.

No fixture contains cookies, actual CSRF/API tokens, PINs, Moonlight endpoints or microphone credentials. Changes in the website contract must fail closed until these selectors and fixtures are reviewed.

History timestamps are display data: `startedAt` preserves the shown date and start time; `finishedAt` preserves the shown end `HH:mm` only. The table supplies no independently verified end date, so overnight intervals are accepted without guessing that date.
