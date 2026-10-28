# Black screen and stream menu diagnostics (0.6.14)

These observations do not change input, recovery, rental, or playback policy.
Logs remain in `switch/fogplay_nx/last-run.log` and the existing `logs/` archive.

## Reproducing on Switch

Use the same stream settings as the failing run (H.265, 720p30, 15 Mbps).
First open and close the menu using Plus + Minus during normal playback. If the
picture goes black, press the same combination once, release both keys, and
allow about five seconds for diagnostics if the application is responsive.
Note whether sound continues and whether the local menu appears. Stop the rental
through the menu when possible; if HOME/force-close is necessary, check the
rental status separately. Never infer server termination from closing the app.

## Menu trace

`Stream menu: event=...` contains a session number, overlay mode, screen number,
focus, the masked Plus/Minus held/down bits, main-loop/handler/paint/present
counters, and recovery/stop/input-gate state. Counters cover the application
lifetime; correlate by session and timestamp. Only Plus/Minus bits are recorded,
not arbitrary game controls, touch coordinates or keyboard contents.

Expected open sequence:

1. `chord-input`: the main loop polled Plus/Minus, before the focus filter.
2. `chord-recognized`: the overlay handler recognized the combination. A
   `Video image at menu:` snapshot records the current uploaded image.
3. `transition-begin` then `transition-ready`: menu state changed and neutral
   remote input was sent. A missing ready event can locate a blocked transition.
4. `paint-begin` then `paint-complete`: the overlay draw path ran.
5. `present-complete-video` or `present-complete-no-video`: SDL_RenderPresent
   returned after the transition, with or without a video texture.

`release-gate` records suppression until controls are released. `active`,
`inactive`, `focus-change` and a five-second `heartbeat` show whether the main
loop remains alive even when the stream handler is skipped. Events are emitted
on transitions/button edges, not on every frame. A missing final event is only
evidence within the retained log: abrupt process termination may lose queued
log writes. `paint-complete` and `present-complete` are API-path evidence, not
proof that the user could see the menu on the physical display.

## Image trace

Periodic and final `Video decoder:` diagnostics include `image_*` fields. The
application samples at most 32 x 18 cell centers of each successfully uploaded
CPU IYUV image, after hardware transfer/conversion. It never reads the GPU
framebuffer and never saves pixels, screenshots or video. The small sample's
average/max cost is recorded as `image_sample_avg_us` / `image_sample_max_us`.

- `image_points`, `image_y_min/max/mean`, `image_dark_points` and
  `image_chroma_deviation` summarize luminance and distance from neutral chroma.
- `image_near_black=1` means all sampled Y values are <=24 and all sampled U/V
  values are within 8 of 128. This intentionally covers full/limited-range dark
  pixels. It is a heuristic, not a colorimetrically exact black classification.
- `image_black_frames/runs/streak/streak_max` retain short dark intervals even
  if they end before the next five-second log. Counts describe uploaded images,
  not received packets or elapsed duration. A size change starts a new run.
- `image_unchanged_streak/max` count successive matching sampled images with
  unchanged dimensions. A static desktop is expected to match. This does not
  prove that every pixel is unchanged or that a stream froze.
- `image_uploaded/drawn/presented_generation` correlate the latest successful
  upload, draw submission and acknowledged new generation after present.
- `image_upload_age_ms`, `image_draw_age_ms`, `image_new_present_age_ms` locate
  stale stages. Age -1 means no successful operation yet; new-present age grows
  when an old texture is repeatedly shown. `image_draws/empty_draws` distinguish
  repeated texture submission from absence of a texture.
- `image_create/upload/clear/copy_errors` count failed SDL operations. A failed
  upload does not overwrite the recorded image; a failed draw cannot acknowledge
  the new image as presented.
- `image_presented_black` describes the last acknowledged generation;
  `image_presented_black_frames` counts each such generation once, not redraws.

Interpret alongside RTP packet/frame freshness, decoder errors, video queue,
output loop and menu events. Advancing uploads with near-black samples place the
dark image at or before CPU texture input; this still cannot distinguish server
content from decode/transfer/conversion faults. Non-black samples with completed
draw/present calls narrow investigation to later rendering/display or unsampled
content. Legitimate dark game scenes and a sparse grid can produce false matches.
No GPU/display readback or server-side capture is implied.
