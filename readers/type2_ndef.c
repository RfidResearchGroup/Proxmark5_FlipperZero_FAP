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

enum {
    WscAuthentication = 0x1003,
    WscCredential = 0x100e,
    WscEncryption = 0x100f,
    WscSsid = 0x1045,
};

static uint16_t read_be16(const uint8_t* bytes) {
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static const uint8_t* wsc_read_attribute(
    const uint8_t** data, size_t* size, uint16_t* type, size_t* length) {
    if(*size < 4) return NULL;
    *type = read_be16(*data);
    *length = read_be16(*data + 2);
    if(*length > *size - 4) return NULL;
    const uint8_t* value = *data + 4;
    *data = value + *length;
    *size -= 4 + *length;
    return value;
}

static const char* wifi_security(uint16_t authentication, uint16_t encryption) {
    switch(authentication) {
    case 0x0001:
        if(encryption == 0x0002) return "WEP (open)";
        return encryption <= 0x0001 ? "Open" : "Unknown";
    case 0x0002:
        return "WPA Personal";
    case 0x0004:
        return "WEP (shared)";
    case 0x0008:
        return "WPA Enterprise";
    case 0x0010:
        return "WPA2 Enterprise";
    case 0x0020:
        return "WPA2 Personal";
    case 0x0022:
        return "WPA/WPA2 Personal";
    default:
        return "Unknown";
    }
}

static Type2NdefResult format_wifi(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    const uint8_t* ssid = NULL;
    size_t ssid_size = 0;
    uint16_t authentication = 0;
    uint16_t encryption = 0;
    while(size) {
        uint16_t type;
        size_t length;
        const uint8_t* value = wsc_read_attribute(&data, &size, &type, &length);
        if(!value) return Type2NdefInvalid;
        switch(type) {
        case WscSsid:
            if(ssid || !length || length > 32) return Type2NdefInvalid;
            ssid = value;
            ssid_size = length;
            break;
        case WscAuthentication:
            if(length != 2) return Type2NdefInvalid;
            authentication = read_be16(value);
            break;
        case WscEncryption:
            if(length != 2) return Type2NdefInvalid;
            encryption = read_be16(value);
            break;
        case WscCredential:
            return Type2NdefInvalid;
        default:
            break;
        }
    }
    if(!ssid || !append_field(text, text_size, "SSID: ", ssid, ssid_size)) return Type2NdefInvalid;
    const char* security = wifi_security(authentication, encryption);
    if(!append_field(text, text_size, "Security: ", (const uint8_t*)security, strlen(security))) {
        return Type2NdefInvalid;
    }
    return Type2NdefWifi;
}

static Type2NdefResult read_wifi(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    const uint8_t* credential = NULL;
    size_t credential_size = 0;
    while(size) {
        uint16_t type;
        size_t length;
        const uint8_t* value = wsc_read_attribute(&data, &size, &type, &length);
        if(!value) return Type2NdefInvalid;
        if(!credential && type == WscCredential) {
            credential = value;
            credential_size = length;
        }
    }
    if(!credential) return Type2NdefOther;
    return format_wifi(credential, credential_size, text, text_size);
}

static Type2NdefResult mime_format(const uint8_t* type, size_t type_size) {
    if(matches_case_insensitive(type, type_size, "text/vcard") ||
       matches_case_insensitive(type, type_size, "text/x-vcard")) {
        return Type2NdefVCard;
    }
    if(matches_case_insensitive(type, type_size, "application/vnd.wfa.wsc")) return Type2NdefWifi;
    return Type2NdefOther;
}

static Type2NdefResult read_ndef_message(
    const uint8_t* data, size_t size, char* text, size_t text_size, bool in_poster) {
    bool first = true;
    bool found_text = false;
    const uint8_t* uri = NULL;
    size_t uri_size = 0;
    const uint8_t* poster = NULL;
    size_t poster_size = 0;
    const uint8_t* mime_payload = NULL;
    size_t mime_size = 0;
    Type2NdefResult mime = Type2NdefOther;
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
        } else if(!in_poster && !mime_payload && tnf == 2) {
            mime = mime_format(data, type_size);
            if(mime != Type2NdefOther) {
                mime_payload = payload;
                mime_size = payload_size;
            }
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
                if(mime == Type2NdefVCard) return read_vcard(mime_payload, mime_size, text, text_size);
                if(mime == Type2NdefWifi) return read_wifi(mime_payload, mime_size, text, text_size);
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

#define SHORT_TLV_MESSAGE_MAX 254U

typedef bool (*NdefMessageMatch)(const uint8_t* data, size_t size);

static bool replace_message(
    uint8_t* data, size_t size, const uint8_t* message, size_t length, NdefMessageMatch matches) {
    if(!length || length > SHORT_TLV_MESSAGE_MAX || length + 3 > size) return false;
    if(!data) return true;
    size_t end = length + 3;
    size_t old_end = 0;
    if(data[0] == 0x03) {
        size_t old_length = data[1];
        old_end = old_length + 3;
        if(old_length > SHORT_TLV_MESSAGE_MAX || old_end > size || data[old_end - 1] != 0xfe) {
            return false;
        }
        if(old_length && !matches(data + 2, old_length)) return false;
    } else if(data[0] == 0xfe) {
        old_end = 1;
    } else {
        for(size_t i = 0; i < size; i++) {
            if(data[i]) return false;
        }
    }

    for(size_t i = old_end; i < end; i++) {
        if(data[i]) return false;
    }
    for(size_t i = end; i < old_end; i++) data[i] = 0;
    data[0] = 0x03;
    data[1] = length;
    memcpy(data + 2, message, length);
    data[end - 1] = 0xfe;
    return true;
}

static bool printable(const char* text) {
    for(; *text; text++) {
        if((unsigned char)*text < 0x20 || (unsigned char)*text > 0x7e) return false;
    }
    return true;
}

static size_t short_record_size(const uint8_t* data, size_t size, uint8_t flags, const char* type) {
    size_t type_size = strlen(type);
    if(size < 3 + type_size || data[0] != flags || data[1] != type_size ||
       data[2] > size - 3 - type_size || memcmp(data + 3, type, type_size)) {
        return 0;
    }
    return 3 + type_size + data[2];
}

static size_t text_record_size(const uint8_t* data, size_t size, uint8_t flags) {
    size_t length = short_record_size(data, size, flags, "T");
    return length > 4 && !(data[4] & 0xc0) && (data[4] & 0x3f) < data[2] ? length : 0;
}

static bool url_record(const uint8_t* data, size_t size, uint8_t flags) {
    return size >= 6 && short_record_size(data, size, flags, "U") == size &&
           data[4] < sizeof(uri_prefixes) / sizeof(uri_prefixes[0]);
}

static bool single_text(const uint8_t* data, size_t size) {
    return text_record_size(data, size, 0xd1) == size;
}

static bool single_url(const uint8_t* data, size_t size) {
    return url_record(data, size, 0xd1);
}

static bool text_and_url(const uint8_t* data, size_t size) {
    size_t text_size = text_record_size(data, size, 0x91);
    return text_size && url_record(data + text_size, size - text_size, 0x51);
}

static bool smart_poster(const uint8_t* data, size_t size) {
    if(short_record_size(data, size, 0xd1, "Sp") != size || size <= 5) return false;
    const uint8_t* inner = data + 5;
    size_t inner_size = size - 5;
    size_t url_size = short_record_size(inner, inner_size, 0x91, "U");
    return url_size && url_record(inner, url_size, 0x91) && url_size < inner_size &&
           text_record_size(inner + url_size, inner_size - url_size, 0x51) == inner_size - url_size;
}

static size_t put_record(
    uint8_t* out, uint8_t flags, const char* type, const void* prefix, size_t prefix_size,
    const void* payload, size_t payload_size) {
    size_t type_size = strlen(type);
    out[0] = flags;
    out[1] = type_size;
    out[2] = prefix_size + payload_size;
    memcpy(out + 3, type, type_size);
    if(prefix_size) memcpy(out + 3 + type_size, prefix, prefix_size);
    memcpy(out + 3 + type_size + prefix_size, payload, payload_size);
    return 3 + type_size + prefix_size + payload_size;
}

static size_t put_text_record(uint8_t* out, uint8_t flags, const char* text, size_t length) {
    static const uint8_t prefix[] = {2, 'e', 'n'};
    return put_record(out, flags, "T", prefix, sizeof(prefix), text, length);
}

static size_t put_url_record(
    uint8_t* out, uint8_t flags, const char* address, size_t length, bool https) {
    // NFC Forum URI RTD prefix codes, also in PM3 client/src/nfc/ndef.c.
    const uint8_t prefix[] = {https ? 0x04 : 0x03};
    return put_record(out, flags, "U", prefix, sizeof(prefix), address, length);
}

bool type2_ndef_replace_text(uint8_t* data, size_t size, const char* text) {
    size_t length = strlen(text);
    if(!length || length > SHORT_TLV_MESSAGE_MAX - 7 || !printable(text)) return false;
    uint8_t message[SHORT_TLV_MESSAGE_MAX];
    size_t used = put_text_record(message, 0xd1, text, length);
    return replace_message(data, size, message, used, single_text);
}

bool type2_ndef_url_address_valid(const char* address) {
    size_t host_size = strcspn(address, "/?#");
    if(!host_size || address[host_size - 1] == ':') return false;
    for(const char* ch = address; *ch; ch++) {
        if((unsigned char)*ch <= 0x20 || (unsigned char)*ch > 0x7e || strchr("\"<>\\^\x60{|}", *ch)) {
            return false;
        }
    }
    return true;
}

bool type2_ndef_replace_url(uint8_t* data, size_t size, const char* address, bool https) {
    size_t length = strlen(address);
    if(length > SHORT_TLV_MESSAGE_MAX - 5 || !type2_ndef_url_address_valid(address)) return false;
    uint8_t message[SHORT_TLV_MESSAGE_MAX];
    size_t used = put_url_record(message, 0xd1, address, length, https);
    return replace_message(data, size, message, used, single_url);
}

bool type2_ndef_replace_text_uri(
    uint8_t* data, size_t size, const char* text, const char* address, bool https, bool poster) {
    size_t text_length = strlen(text);
    size_t address_length = strlen(address);
    size_t overhead = poster ? 17 : 12;
    if(!text_length || text_length > SHORT_TLV_MESSAGE_MAX - overhead ||
       address_length > SHORT_TLV_MESSAGE_MAX - overhead - text_length || !printable(text) ||
       !type2_ndef_url_address_valid(address)) {
        return false;
    }
    uint8_t message[SHORT_TLV_MESSAGE_MAX];
    if(poster) {
        // NFC Forum Smart Poster RTD wraps the URI and title in an Sp record.
        size_t used = put_url_record(message + 5, 0x91, address, address_length, https);
        used += put_text_record(message + 5 + used, 0x51, text, text_length);
        message[0] = 0xd1;
        message[1] = 2;
        message[2] = used;
        memcpy(message + 3, "Sp", 2);
        return replace_message(data, size, message, used + 5, smart_poster);
    }
    size_t used = put_text_record(message, 0x91, text, text_length);
    used += put_url_record(message + used, 0x51, address, address_length, https);
    return replace_message(data, size, message, used, text_and_url);
}
