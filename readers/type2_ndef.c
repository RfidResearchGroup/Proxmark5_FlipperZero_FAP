#include "type2_ndef.h"
#include <stdbool.h>
#include <string.h>

static const char* const uri_prefixes[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:",
    "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://",
    "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:",
    "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
    "tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:",
    "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:",
};

static Type2NdefResult read_uri(
    const uint8_t* payload, size_t size, char* text, size_t text_size) {
    if(size < 2) return Type2NdefInvalid;
    if(payload[0] >= sizeof(uri_prefixes) / sizeof(uri_prefixes[0])) return Type2NdefOther;
    const char* prefix = uri_prefixes[payload[0]];
    size_t prefix_size = strlen(prefix);
    if(prefix_size >= text_size || size > text_size - prefix_size) return Type2NdefInvalid;
    for(size_t i = 1; i < size; i++) {
        if(payload[i] < 0x20 || payload[i] == 0x7f) return Type2NdefInvalid;
    }
    memcpy(text, prefix, prefix_size);
    memcpy(text + prefix_size, payload + 1, size - 1);
    text[prefix_size + size - 1] = '\0';
    return Type2NdefUri;
}

static Type2NdefResult show_text_and_uri(
    const uint8_t* uri, size_t uri_size, char* text, size_t text_size) {
    const char heading[] = "\n\nURL\n";
    size_t used = strlen(text);
    size_t uri_offset = used + sizeof(heading) - 1;
    if(uri_offset < text_size &&
       read_uri(uri, uri_size, text + uri_offset, text_size - uri_offset) == Type2NdefUri) {
        memcpy(text + used, heading, sizeof(heading) - 1);
        return Type2NdefTextAndUri;
    }
    return Type2NdefText;
}

static Type2NdefResult read_ndef_message(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    bool first = true;
    bool found_text = false;
    const uint8_t* uri = NULL;
    size_t uri_size = 0;
    while(size >= 2) {
        uint8_t flags = *data++;
        uint8_t type_size = *data++;
        size -= 2;
        if(((flags & 0x80) != 0) != first) return Type2NdefInvalid;
        if(flags & 0x20) return Type2NdefOther; // Chunked records need reassembly.
        first = false;

        size_t width = (flags & 0x10) ? 1 : 4;
        if(size < width) return Type2NdefInvalid;

        uint32_t payload_size = 0;
        for(size_t index = 0; index < width; index++) {
            payload_size = (payload_size << 8) | *data++;
        }
        size -= width;

        uint8_t id_size = 0;
        if(flags & 0x08) {
            if(!size) return Type2NdefInvalid;
            id_size = *data++;
            size--;
        }

        size_t header_size = (size_t)type_size + id_size;
        if(header_size > size || payload_size > size - header_size) return Type2NdefInvalid;
        uint8_t tnf = flags & 0x07;
        if(tnf > 5 || (!tnf && (header_size || payload_size))) return Type2NdefInvalid;
        const uint8_t* payload = data + header_size;
        if(tnf == 1 && type_size == 1 && data[0] == 'T') {
            if(!payload_size || (payload[0] & 0x40)) return Type2NdefInvalid;
            size_t language_size = payload[0] & 0x3f;
            if(language_size >= payload_size) return Type2NdefInvalid;
            if(!found_text && !(payload[0] & 0x80)) {
                size_t count = payload_size - 1 - language_size;
                if(count >= text_size) return Type2NdefInvalid;
                for(size_t i = 0; i < count; i++) {
                    uint8_t byte = payload[1 + language_size + i];
                    text[i] = ((byte < 0x20 && byte != '\n') || byte == 0x7f) ? '.' : byte;
                }
                text[count] = '\0';
                found_text = true;
            }
        } else if(tnf == 1 && type_size == 1 && data[0] == 'U' && !uri) {
            uri = payload;
            uri_size = payload_size;
        }

        data = payload + payload_size;
        size -= header_size + payload_size;
        if(flags & 0x40) {
            if(size) return Type2NdefInvalid;
            if(found_text) {
                return uri ? show_text_and_uri(uri, uri_size, text, text_size) : Type2NdefText;
            }
            if(uri) return read_uri(uri, uri_size, text, text_size);
            return Type2NdefOther;
        }
    }
    return Type2NdefInvalid;
}

Type2NdefResult type2_ndef_read(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    if(!text_size) return Type2NdefInvalid;
    text[0] = '\0';
    for(size_t offset = 0; offset < size;) {
        uint8_t type = data[offset++];
        if(type == 0x00) continue;
        if(type == 0xfe) break;
        if(offset == size) return Type2NdefInvalid;

        size_t length = data[offset++];
        if(length == 0xff) {
            if(size - offset < 2) return Type2NdefInvalid;
            length = ((size_t)data[offset] << 8) | data[offset + 1];
            offset += 2;
        }
        if(length > size - offset) return Type2NdefInvalid;
        if(type == 0x03) {
            if(!length) return Type2NdefEmpty;
            Type2NdefResult result = read_ndef_message(data + offset, length, text, text_size);
            if(result == Type2NdefEmpty || result == Type2NdefOther || result == Type2NdefInvalid) {
                text[0] = '\0';
            }
            return result;
        }
        offset += length;
    }
    return Type2NdefEmpty;
}
