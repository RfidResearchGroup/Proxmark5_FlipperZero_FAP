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

static Type2NdefResult read_mime(
    const char* type, const void* payload, size_t size, char* text, size_t text_size) {
    size_t type_size = strlen(type);
    assert(3 + type_size + size < 0xff);
    uint8_t data[256] = {3, 3 + type_size + size, 0xd2, type_size, size};
    memcpy(data + 5, type, type_size);
    memcpy(data + 5 + type_size, payload, size);
    return type2_ndef_read(data, 5 + type_size + size, text, text_size);
}

int main(void) {
    char text[900];
    const uint8_t tag[] = {
        0x00, 0x03, 0x0c, 0xd1, 1, 8, 'T', 2, 'e', 'n', 'H', 'e', 'l', 'l', 'o', 0xfe,
    };
    assert(type2_ndef_read(tag, sizeof(tag), text, sizeof(text)) == Type2NdefText);
    assert(strcmp(text, "Hello") == 0);
    for(size_t size = 2; size < sizeof(tag) - 1; size++) {
        assert(type2_ndef_read(tag, size, text, sizeof(text)) == Type2NdefInvalid);
        assert(text[0] == '\0');
    }
    assert(type2_ndef_read(tag, sizeof(tag), text, 4) == Type2NdefInvalid);

    const uint8_t empty[] = {0x03, 0, 0xfe};
    const uint8_t uri[] = {0x03, 8, 0xd1, 1, 4, 'U', 4, 'a', '.', 'b'};
    const uint8_t uncompressed[] = {
        0x03, 16, 0xd1, 1, 12, 'U', 0, 'h', 't', 't', 'p', 's', ':', '/', '/', 'a', '.', 'b',
    };
    const uint8_t unknown_prefix[] = {0x03, 6, 0xd1, 1, 2, 'U', 0x24, 'a'};
    const uint8_t empty_uri[] = {0x03, 5, 0xd1, 1, 1, 'U', 0};
    const uint8_t bad_uri[] = {0x03, 6, 0xd1, 1, 2, 'U', 4, 0};
    const uint8_t mixed[] = {
        0x03, 20, 0x91, 1, 4, 'U', 4, 'a', '.', 'b',
        0x51, 1, 8, 'T', 2, 'e', 'n', 'H', 'e', 'l', 'l', 'o',
    };
    const uint8_t invalid[] = {0x03, 5, 0xd1, 1, 1, 'T', 7};
    assert(type2_ndef_read(empty, sizeof(empty), text, sizeof(text)) == Type2NdefEmpty);
    assert(type2_ndef_read(uri, sizeof(uri), text, 12) == Type2NdefUri);
    assert(strcmp(text, "https://a.b") == 0);
    assert(type2_ndef_read(uri, sizeof(uri), text, 11) == Type2NdefInvalid);
    assert(text[0] == '\0');
    assert(type2_ndef_read(uncompressed, sizeof(uncompressed), text, sizeof(text)) == Type2NdefUri);
    assert(strcmp(text, "https://a.b") == 0);
    assert(type2_ndef_read(mixed, sizeof(mixed), text, sizeof(text)) == Type2NdefTextAndUri);
    assert(strcmp(text, "Hello\n\nURL\nhttps://a.b") == 0);
    assert(type2_ndef_read(mixed, sizeof(mixed), text, 6) == Type2NdefText);
    assert(strcmp(text, "Hello") == 0);
    assert(type2_ndef_read(unknown_prefix, sizeof(unknown_prefix), text, sizeof(text)) == Type2NdefOther);
    assert(text[0] == '\0');
    assert(type2_ndef_read(empty_uri, sizeof(empty_uri), text, sizeof(text)) == Type2NdefInvalid);
    assert(type2_ndef_read(bad_uri, sizeof(bad_uri), text, sizeof(text)) == Type2NdefInvalid);
    assert(type2_ndef_read(invalid, sizeof(invalid), text, sizeof(text)) == Type2NdefInvalid);

    // Fill the NTAG216 NDEF area, using the longest prefix.
    uint8_t long_uri[872] = {0x03, 0xff, 0x03, 0x64, 0xc1, 1, 0, 0, 0x03, 0x5d, 'U', 7};
    memset(long_uri + 12, 'a', sizeof(long_uri) - 12);
    assert(type2_ndef_read(long_uri, sizeof(long_uri), text, sizeof(text)) == Type2NdefUri);
    assert(strncmp(text, "ftp://anonymous:anonymous@", 26) == 0);
    assert(strlen(text) == 26 + sizeof(long_uri) - 12);

    const uint8_t poster[] = {
        0x03, 25, 0xd1, 2, 20, 'S', 'p', 0x91, 1, 4, 'U', 4, 'a', '.', 'b',
        0x51, 1, 8, 'T', 2, 'e', 'n', 'H', 'e', 'l', 'l', 'o',
    };
    assert(type2_ndef_read(poster, sizeof(poster), text, sizeof(text)) == Type2NdefSmartPoster);
    assert(strcmp(text, "Hello\n\nURL\nhttps://a.b") == 0);
    const uint8_t poster_no_title[] = {
        0x03, 13, 0xd1, 2, 8, 'S', 'p', 0xd1, 1, 4, 'U', 4, 'a', '.', 'b',
    };
    assert(type2_ndef_read(poster_no_title, sizeof(poster_no_title), text, sizeof(text)) ==
           Type2NdefSmartPoster);
    assert(strcmp(text, "https://a.b") == 0);
    const uint8_t poster_no_uri[] = {
        0x03, 17, 0xd1, 2, 12, 'S', 'p', 0xd1, 1, 8, 'T', 2, 'e', 'n', 'H', 'e', 'l', 'l', 'o',
    };
    assert(type2_ndef_read(poster_no_uri, sizeof(poster_no_uri), text, sizeof(text)) ==
           Type2NdefInvalid);
    assert(text[0] == '\0');
    const uint8_t poster_two_uris[] = {
        0x03, 21, 0xd1, 2, 16, 'S', 'p', 0x91, 1, 4, 'U', 4, 'a', '.', 'b',
        0x51, 1, 4, 'U', 4, 'b', '.', 'a',
    };
    assert(type2_ndef_read(poster_two_uris, sizeof(poster_two_uris), text, sizeof(text)) ==
           Type2NdefInvalid);

    const char vcard[] =
        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Alex\\, Example\r\n"
        "group.TEL;LABEL=\"desk: second floor\":tel:+1-202-555-0142\r\n"
        "EMAIL:alex@exa\r\n mple.com\r\nEMAIL:second@example.com\r\nEND:VCARD\r\n";
    const char contact[] = "Name: Alex, Example\nTel: +1-202-555-0142\nEmail: alex@example.com";
    assert(read_mime("text/vcard", vcard, sizeof(vcard) - 1, text, sizeof(text)) == Type2NdefVCard);
    assert(strcmp(text, contact) == 0);
    assert(read_mime("Text/X-vCard", vcard, sizeof(vcard) - 1, text, sizeof(text)) == Type2NdefVCard);
    assert(strcmp(text, contact) == 0);
    assert(read_mime("text/vcard", vcard, sizeof(vcard) - 4, text, sizeof(text)) == Type2NdefInvalid);
    assert(text[0] == '\0');
    assert(read_mime("text/vcard", vcard, sizeof(vcard) - 1, text, 6) == Type2NdefInvalid);
    assert(text[0] == '\0');
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
