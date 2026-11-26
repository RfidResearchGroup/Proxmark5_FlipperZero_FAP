#include <furi.h>
#include "hf_waveform.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"

#define SELECT_TIMEOUT_MS 2500U
#define WAVEFORM_TIMEOUT_MS 4000U

int hf_waveform_capture(uint8_t samples[HF_WAVEFORM_COUNT]) {
    iso14a_raw_cmd_t select = {.flags = ISO14A_CONNECT | ISO14A_CLEARTRACE};
    PacketResponseNG reply;
    clearCommandBuffer();
    if(!SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&select, sizeof(select))) return PM3_EIO;
    if(!WaitForResponseTimeout(CMD_HF_ISO14443A_READER, &reply, SELECT_TIMEOUT_MS)) {
        return PM3_ETIMEOUT;
    }
    if(reply.status != PM3_SUCCESS) return reply.status;
    if(reply.length != sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) return PM3_ESOFT;

    if(!proxmark5_com_prepare_large_reply(HF_WAVEFORM_BUFFER_SIZE)) return PM3_EMALLOC;
    proxmark5_frame_collect_download(CMD_FPGAMEM_DOWNLOADED, samples, HF_WAVEFORM_COUNT);
    int status = PM3_EIO;
    if(SendCommandNG(CMD_FPGAMEM_DOWNLOAD, NULL, 0)) {
        status = proxmark5_frame_wait(CMD_FPGAMEM_DOWNLOAD, &reply, WAVEFORM_TIMEOUT_MS, NULL);
    }
    size_t length;
    int data_status = proxmark5_frame_finish_download(&length);
    proxmark5_com_release_large_reply();

    if(status == PM3_SUCCESS) {
        status = reply.status;
        if(status == PM3_SUCCESS && data_status != PM3_SUCCESS) status = data_status;
        if(status == PM3_SUCCESS &&
           (reply.length != 2 * sizeof(uint32_t) || reply.data.asDwords[0] != HF_WAVEFORM_COUNT ||
            length != HF_WAVEFORM_COUNT)) {
            status = PM3_ESOFT;
        }
    }
    return status;
}
