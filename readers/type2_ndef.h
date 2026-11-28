#ifndef TYPE2_NDEF_H
#define TYPE2_NDEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    Type2NdefEmpty,
    Type2NdefText,
    Type2NdefUri,
    Type2NdefTextAndUri,
    Type2NdefSmartPoster,
    Type2NdefVCard,
    Type2NdefWifi,
    Type2NdefOther,
    Type2NdefInvalid,
} Type2NdefResult;

Type2NdefResult type2_ndef_read(
    const uint8_t* data, size_t size, char* text, size_t text_size);

bool type2_ndef_replace_text(uint8_t* data, size_t size, const char* text);
bool type2_ndef_url_address_valid(const char* address);
bool type2_ndef_replace_url(uint8_t* data, size_t size, const char* address, bool https);

bool type2_ndef_replace_text_uri(
    uint8_t* data, size_t size, const char* text, const char* address, bool https, bool poster);

bool type2_ndef_replace_vcard(
    uint8_t* data, size_t size, const char* name, const char* phone, const char* email);

#define TYPE2_NDEF_WIFI_SSID_MAX 32U
#define TYPE2_NDEF_WIFI_KEY_MAX 64U

bool type2_ndef_wifi_key_valid(const char* key);
bool type2_ndef_replace_wifi(uint8_t* data, size_t size, const char* ssid, const char* key);

#endif
