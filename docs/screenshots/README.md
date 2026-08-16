# Screenshots (menu map)

Folder layout mirrors the FAP UI in `pages/operate_page.c`:

```
Proxmark5
├── PM5 Tools
│   ├── Ping
│   └── HW Version
└── HF RFID
    ├── 14a Reader
    └── MFC Autopwn
        ├── Wizard (dict → nonce → dump)
        ├── Dict
        ├── After dict (Next / Dump)
        ├── Nonces → .nested.log
        ├── Dump (OK / partial / Save)
        └── MFKey (external FAP handoff)
```

| Path | Screen |
|------|--------|
| `00-hardware/` | *(add)* Flipper Zero + Proxmark5 over Type-C CEP |
| `01-proxmark5/` | Root menu |
| `02-pm5-tools/menu/` | PM5 Tools submenu |
| `02-pm5-tools/ping/` | Ping OK / SPI link alive |
| `02-pm5-tools/hw-version/` | MCU / chip info |
| `03-hf-rfid/14a-reader/` | ISO14443A reader |
| `03-hf-rfid/mfc-autopwn/01-wizard/` | Wizard intro |
| `03-hf-rfid/mfc-autopwn/02-dict/` | Dictionary attack progress |
| `03-hf-rfid/mfc-autopwn/03-after-dict/` | Choice after dict |
| `03-hf-rfid/mfc-autopwn/04-nonces/` | Nonce collect → `.nested.log` |
| `03-hf-rfid/mfc-autopwn/05-dump/` | Sector dump / Autopwn OK |
| `03-hf-rfid/mfc-autopwn/06-mfkey/` | Launch MFKey |
| `_misc-pc-client/` | Optional PC `pm3` client shots (not in FAP menus) |

UIDs visible in shots are from **owned test cards** used during development.
