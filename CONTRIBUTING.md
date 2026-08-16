# Contributing

Thanks for helping with experimental **Flipper Zero ↔ Proxmark5 (Type-C CEP)** work.

## Repositories

| Piece | Repo / branch |
|-------|----------------|
| Flipper FAP | https://github.com/limerx/Proxmark5_FlipperZero_FAP (`f0-cep-autopwn`) |
| PM5 firmware (CEP) | https://github.com/limerx/proxmark3 (`pm5-f0-cep`) |

You need **both** for a working CEP stack. Stock upstream firmware often leaves CEP disabled → FAP stays on “Connecting…”.

## Safety

- Flash **Proxmark5 firmware only onto a Proxmark5**.
- Never flash PM5 images onto a Flipper (check USB `ID_MODEL` first).
- Use only tags / systems you own or are authorized to test.

## Build (FAP)

```bash
git clone -b f0-cep-autopwn https://github.com/limerx/Proxmark5_FlipperZero_FAP.git
cd Proxmark5_FlipperZero_FAP
ufbt
ufbt launch
```

Momentum-class (or compatible) Flipper firmware recommended.

## Build (PM5 CEP firmware)

Follow DXL / RRG Proxmark5 build docs for this tree, on branch `pm5-f0-cep`.  
See [`docs/F0_CEP_FLIPPER.md`](https://github.com/limerx/proxmark3/blob/pm5-f0-cep/docs/F0_CEP_FLIPPER.md) in the firmware fork.

## How to send changes

1. Fork the relevant repo.
2. Branch from `f0-cep-autopwn` (FAP) or `pm5-f0-cep` (firmware).
3. Keep commits focused; credit upstream (DXL / RRG / Iceman) where appropriate.
4. Open a PR against **this fork** (not necessarily upstream yet — work is experimental).
5. Describe hardware (Flipper FW + PM5) and what you tested (Ping, HW Version, Autopwn, etc.).

## Issues

Use GitHub Issues on the repo that matches the bug (FAP UI vs PM5 CEP firmware).  
Include: steps, expected vs actual, firmware versions, and whether CEP handshake / Ping succeeds.

## License

GPL-3.0 — see `LICENSE`. Keep attribution in `NOTICE` / `application.fam` / source headers.
