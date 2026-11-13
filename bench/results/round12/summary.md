
App-matrix baseline; KiB and seconds

| case / metric | glibc | v2 | jemalloc | mimalloc |
|---|---|---|---|---|
| lua / rss | 10,388 (10,368–10,416) | 17,828 (17,496–18,216) | 14,600 (14,592–14,652) | 11,028 (11,024–11,036) |
| lua / seconds | 0.471 (0.452–0.511) | 0.384 (0.353–0.400) | 0.429 (0.393–0.452) | 0.381 (0.361–0.430) |

Mechanism comparison; KiB and seconds

| case / metric | base | cold |
|---|---|---|
| lua / rss | 17,896 (17,800–17,960) | 14,844 (14,772–15,220) |
| lua / seconds | 0.385 (0.373–0.395) | 0.389 (0.383–0.397) |

Guard workloads; KiB and seconds

| case / metric | base | cold |
|---|---|---|
| bursty / rss | 42,912 (42,280–43,188) | 42,932 (42,208–43,212) |
| bursty / seconds | 7.610 (7.527–7.697) | 7.656 (7.559–7.705) |
| consprobe / rss | 7,872 (7,848–7,872) | 7,880 (7,880–7,880) |
| consprobe / seconds | 3.535 (3.531–3.559) | 3.532 (3.524–3.543) |
| local / seconds | 0.436 (0.430–0.458) | 0.428 (0.425–0.464) |
| cross / seconds | 0.313 (0.219–0.318) | 0.342 (0.239–0.502) |
| many / seconds | 1.019 (0.858–1.068) | 1.238 (1.039–1.441) |
| census / seconds | 2.873 (2.700–3.488) | 2.978 (2.677–3.240) |
