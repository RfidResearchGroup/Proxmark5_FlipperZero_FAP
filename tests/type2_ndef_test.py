#!/usr/bin/env python3
"""Check the Type 2 NDEF parser with complete and truncated records."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

READERS = Path(__file__).resolve().parents[1] / "readers"
TEST = r'''
#include <assert.h>
#include <string.h>
#include "type2_ndef.h"

int main(void) {
    char text[145];
    const uint8_t tag[] = {
        0x00, 0x03, 0x0c, 0xd1, 1, 8, 'T', 2, 'e', 'n', 'H', 'e', 'l', 'l', 'o', 0xfe,
    };
    assert(type2_ndef_read_text(tag, sizeof(tag), text, sizeof(text)) == Type2NdefText);
    assert(strcmp(text, "Hello") == 0);
    for(size_t size = 2; size < sizeof(tag) - 1; size++) {
        assert(type2_ndef_read_text(tag, size, text, sizeof(text)) == Type2NdefInvalid);
        assert(text[0] == '\0');
    }
    assert(type2_ndef_read_text(tag, sizeof(tag), text, 4) == Type2NdefInvalid);

    const uint8_t empty[] = {0x03, 0, 0xfe};
    const uint8_t uri[] = {0x03, 5, 0xd1, 1, 1, 'U', 0};
    const uint8_t invalid[] = {0x03, 5, 0xd1, 1, 1, 'T', 7};
    assert(type2_ndef_read_text(empty, sizeof(empty), text, sizeof(text)) == Type2NdefEmpty);
    assert(type2_ndef_read_text(uri, sizeof(uri), text, sizeof(text)) == Type2NdefOther);
    assert(type2_ndef_read_text(invalid, sizeof(invalid), text, sizeof(text)) == Type2NdefInvalid);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="pm5-ndef-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(TEST)
    executable = temp / "test"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(
        compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(READERS),
                    str(READERS / "type2_ndef.c"), str(temp / "test.c"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
