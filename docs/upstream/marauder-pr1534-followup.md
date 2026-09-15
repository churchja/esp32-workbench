<!--
POSTED 2026-09-15T10:00:30Z by churchja on upstream PR #1534:
https://github.com/justcallmekoko/ESP32Marauder/pull/1534#issuecomment-5678286189
Verified identical to this file by reading it back from the API.
Record of what was sent; edit upstream, not this file.
-->

Quick update, since v1.16.0 has shipped.

**v1.16.0 still has the problem.** The shared branch is unchanged, now at
`configs.h:2879`, and resolving it with `gcc -E -dM -x c++ -D<TARGET>` still
gives `HAS_MAX1704X` for `MARAUDER_V4`, `MARAUDER_V6` and `MARAUDER_V6_1`. So a
V6 or V6.1 updated to the current release loses its battery meter. This PR is
still mergeable against master and every CI build passes.

Two things I found while re-checking, in case they help decide whether the
V4/V6/V6.1 change is safe:

- #1392 is about v7 and doesn't mention V4, V6, V6.1 or KIT. That looks like
  those four were changed in the same edit rather than because they have a
  MAX17048. If I've misread it and some of that hardware does have one, that
  changes the right fix, and I'd rather know.
- Every V6/V6.1 owner who has reported this since v1.14.0 says the meter broke:
  Clawzman, jirjirjir and hscrystal on #1403, stevedee78 on both issues, and my
  own board. I couldn't find anyone saying v1.14.0 *fixed* their meter, which
  I'd expect to see if many of these boards had a MAX17048.

**The one case I can't confirm:** the v1.16.0 release notes list `_v6_1.bin` for
**AWOK V2/V3** as well as v6.1/v6.2. I don't have an AWOK board, so I can't tell
you which gauge it uses. If it's a MAX17048, this change would break the meter on
those boards, and `MARAUDER_V6_1` would need splitting by vendor instead. The
only AWOK data point I found is #1326, tested on an AWOK Dual V3 with the IP5306
setup. It only says the device booted, though, not that the battery reading worked.

If someone with an AWOK V2/V3 can check its Device Info page on v1.13.0 or
earlier, that would settle it. Happy to rebase, split further, or narrow this to
V6/V6.1 only if you'd prefer.
