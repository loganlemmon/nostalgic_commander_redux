# Known Issues

Bugs and suspect behavior, ordered roughly by user impact. Approved upcoming
work lives in [TODOs.md](TODOs.md); unvetted ideas in [IDEAS.md](IDEAS.md).

## BPM trails the Health app — accepted, not a bug

`update_health_info()` reads `HealthMetricHeartRateBPM`, which the SDK defines as
"a filtered value that is at most 15 minutes old", and the system's default HRM
sample period is 10 minutes. The Health app looks live because a foreground app
can request 1-second sampling via `health_service_set_heart_rate_sample_period()`.

Deliberately not fixed. The filtered value is the right glanceable number — it
drops bad readings from hand movement and poor sensor contact — and a watchface
is displayed all day, so requesting faster sampling would run the HRM
continuously and cost battery. The refresh path is not at fault: health data for
every displayed metric is re-read every tick and on `HealthEventHeartRateUpdate`.

## CRT degauss sound crackles on hardware during the strike — firmware

On stock PT2 firmware, animating frames while the strike sound plays starves
the audio refill path (128 ms ring, droppable system-task refills,
KernelMain/KernelBG priority inversion). The watchface side mitigates: the
sound is a 110 ms thunk that fits the ring and starts after the wake frame.
Full analysis and firmware fix candidates:
[2026-08-23-audio-underrun](docs/superpowers/problems/2026-08-23-audio-underrun.md).
