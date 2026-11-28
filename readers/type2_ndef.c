#include "type2_ndef.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static Type2NdefResult append_uri(
    const uint8_t* uri, size_t uri_size, char* text, size_t text_size) {
    const char heading[] = "\n\nURL\n";
    size_t used = strlen(text);
    size_t uri_offset = used + sizeof(heading) - 1;
    if(uri_offset >= text_size) return Type2NdefInvalid;
    Type2NdefResult result = read_uri(uri, uri_size, text + uri_offset, text_size - uri_offset);
    if(result == Type2NdefUri) memcpy(text + used, heading, sizeof(heading) - 1);
    return result;
}

static bool append_field(
    char* text, size_t text_size, const char* label, const uint8_t* value, size_t value_size) {
    size_t used = strlen(text);
    size_t label_size = strlen(label);
    if((used != 0) + label_size + value_size >= text_size - used) return false;
    if(used) text[used++] = '\n';
    memcpy(text + used, label, label_size);
    used += label_size;
    for(size_t i = 0; i < value_size; i++) {
        uint8_t byte = value[i];
        text[used++] = (byte < 0x20 || byte == 0x7f) ? '.' : byte;
    }
    text[used] = '\0';
    return true;
}

static bool matches_case_insensitive(const uint8_t* bytes, size_t size, const char* expected) {
    return size == strlen(expected) && strncasecmp((const char*)bytes, expected, size) == 0;
}

static char* vcard_value(char* line) {
    bool quoted = false;
    for(char* ch = line; *ch; ch++) {
        if(*ch == '"') quoted = !quoted;
        if(*ch == ':' && !quoted) return ch + 1;
    }
    return NULL;
}

static void vcard_unescape(char* value) {
    char* out = value;
    for(const char* ch = value; *ch; ch++) {
        if(*ch == '\\' && ch[1] && strchr("\\,;nN", ch[1])) {
            ch++;
            *out++ = (*ch == 'n' || *ch == 'N') ? ' ' : *ch;
        } else {
            *out++ = *ch;
        }
    }
    *out = '\0';
}

static Type2NdefResult format_vcard(char* card, char* text, size_t text_size) {
    static const char* const names[] = {"FN", "TEL", "EMAIL"};
    static const char* const labels[] = {"Name: ", "Tel: ", "Email: "};
    bool shown[sizeof(names) / sizeof(names[0])] = {false};
    char* line = strchr(card, '\n');
    if(!line) return Type2NdefInvalid;
    *line++ = '\0';
    if(strcasecmp(card, "BEGIN:VCARD") != 0) return Type2NdefInvalid;

    while(line) {
        char* next = strchr(line, '\n');
        if(next) *next++ = '\0';
        if(strcasecmp(line, "END:VCARD") == 0) {
            return *text ? Type2NdefVCard : Type2NdefOther;
        }
        char* value = vcard_value(line);
        if(value && *value) {
            size_t key_size = strcspn(line, ";:");
            const char* key = line;
            for(size_t i = 0; i < key_size; i++) {
                if(line[i] == '.') key = line + i + 1;
            }
            key_size -= key - line;
            for(size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); i++) {
                if(shown[i] || !matches_case_insensitive((const uint8_t*)key, key_size, names[i])) {
                    continue;
                }
                if(i == 1 && strncasecmp(value, "tel:", 4) == 0) value += 4;
                if(!*value) break;
                vcard_unescape(value);
                if(!append_field(text, text_size, labels[i], (const uint8_t*)value, strlen(value))) {
                    return Type2NdefInvalid;
                }
                shown[i] = true;
                break;
            }
        }
        line = next;
    }
    return Type2NdefInvalid;
}

static Type2NdefResult read_vcard(
    const uint8_t* payload, size_t size, char* text, size_t text_size) {
    if(!size || memchr(payload, 0, size)) return Type2NdefInvalid;
    char* card = malloc(size + 1);
    if(!card) return Type2NdefInvalid;
    size_t used = 0;
    for(size_t i = 0; i < size; i++) {
        if(payload[i] == '\r' && i + 1 < size && payload[i + 1] == '\n') continue;
        if(payload[i] == '\n' && i + 1 < size && (payload[i + 1] == ' ' || payload[i + 1] == '\t')) {
            i++;
            continue;
        }
        card[used++] = payload[i];
    }
    card[used] = '\0';
    Type2NdefResult result = format_vcard(card, text, text_size);
    free(card);
    return result;
}

static Type2NdefResult read_ndef_message(
    const uint8_t* data, size_t size, char* text, size_t text_size, bool in_poster) {
    bool first = true;
    bool found_text = false;
    const uint8_t* uri = NULL;
    size_t uri_size = 0;
    const uint8_t* poster = NULL;
    size_t poster_size = 0;
    const uint8_t* vcard = NULL;
    size_t vcard_size = 0;
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
        } else if(tnf == 1 && type_size == 1 && data[0] == 'U') {
            if(in_poster && uri) return Type2NdefInvalid;
            if(!uri) {
                uri = payload;
                uri_size = payload_size;
            }
        } else if(!in_poster && !poster && tnf == 1 && type_size == 2 && memcmp(data, "Sp", 2) == 0) {
            poster = payload;
            poster_size = payload_size;
        } else if(
            !in_poster && !vcard && tnf == 2 &&
            (matches_case_insensitive(data, type_size, "text/vcard") ||
             matches_case_insensitive(data, type_size, "text/x-vcard"))) {
            vcard = payload;
            vcard_size = payload_size;
        }

        data = payload + payload_size;
        size -= header_size + payload_size;
        if(flags & 0x40) {
            if(size) return Type2NdefInvalid;
            if(poster) {
                text[0] = '\0';
                return read_ndef_message(poster, poster_size, text, text_size, true);
            }
            if(!uri) {
                if(in_poster) return Type2NdefInvalid;
                if(found_text) return Type2NdefText;
                if(vcard) return read_vcard(vcard, vcard_size, text, text_size);
                return Type2NdefOther;
            }
            Type2NdefResult result = found_text ? append_uri(uri, uri_size, text, text_size) :
                                                 read_uri(uri, uri_size, text, text_size);
            if(in_poster) return result == Type2NdefUri ? Type2NdefSmartPoster : result;
            if(found_text) return result == Type2NdefUri ? Type2NdefTextAndUri : Type2NdefText;
            return result;
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
            Type2NdefResult result = read_ndef_message(data + offset, length, text, text_size, false);
            if(result == Type2NdefEmpty || result == Type2NdefOther || result == Type2NdefInvalid) {
                text[0] = '\0';
            }
            return result;
        }
        offset += length;
    }
    return Type2NdefEmpty;
}
