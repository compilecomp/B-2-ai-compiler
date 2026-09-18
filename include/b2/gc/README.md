# `include/b2/gc/` — GC team public API (v0 stub)

**Status:** v0 stub. **No headers.** See `docs/gc.md`.

This directory is the gc team's public header surface. It is empty
today; when the gc team ships v1, headers describing `Region`,
`MemRegion`, `Barrier`, `Collector`, `HandleScope`, etc. will live
here under the include path `b2/gc/*`.

Consumers (interpreter, codegen, AOT) will `#include <b2/gc/...>` to
access write-barrier fast paths, safepoint hooks, and pin/unpin APIs.
The header guard convention is `B2_GC_<NAME>_H` and the namespace is
`b2::gc`.

Until v1 lands, do not add headers here without an INFO message and
cross-team ADVISORY per `docs/teams/messaging.md`.
