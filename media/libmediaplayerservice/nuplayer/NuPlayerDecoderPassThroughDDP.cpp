/**
 *  Copyright 2017 NXP
 *  All Rights Reserved.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerDecoderPassThroughDDP"
#include <utils/Log.h>
#include <inttypes.h>

#include "NuPlayerDecoderPassThroughDDP.h"

#include "NuPlayerRenderer.h"
#include "NuPlayerSource.h"

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaDefs.h>

#include "ATSParser.h"

namespace android {

#define AC3_FRAME_HEAD_SIZE  8
#define NFSCOD      3   /* # defined sample rates */
#define NDATARATE   38  /* # defined data rates */
#define AC3D_FRAME_SIZE 1536

NuPlayer::DecoderPassThroughDDP::DecoderPassThroughDDP(
        const sp<AMessage> &notify,
        const sp<Source> &source,
        const sp<Renderer> &renderer)
    : DecoderPassThrough(notify,source,renderer){
    ALOGW_IF(renderer == NULL, "expect a non-NULL renderer");
}
NuPlayer::DecoderPassThroughDDP::~DecoderPassThroughDDP()
{
}
bool NuPlayer::DecoderPassThroughDDP::enableOffload()
{
    return false;
}
int32_t NuPlayer::DecoderPassThroughDDP::getAudioOutputFlags()
{
    ALOGV("getAudioOutputFlags AUDIO_OUTPUT_FLAG_DIRECT");
    return AUDIO_OUTPUT_FLAG_DIRECT;
}
//assume one buffer in one unit
status_t NuPlayer::DecoderPassThroughDDP::parseAccessUnit(sp<ABuffer> *accessUnit)
{

    sp<ABuffer> src = *accessUnit;
    sp<ABuffer> tar;
    int64_t timeUs;
    uint16_t header1=0xF872;
    uint16_t header2=0x4E1F;
    uint16_t header3=0x15;
    uint16_t header4;
    uint32_t fromSize = src->size();
    uint8_t* src_ptr= NULL;
    uint8_t* tar_ptr= NULL;

    //skip empty buffer
    if(fromSize == 0)
        return OK;

    ALOGV("fromSize=%d",fromSize);

    uint8_t first = *src->data();

    #define OUTPUT_BUFFER_SIZE (6144*4)

    if(fromSize > OUTPUT_BUFFER_SIZE)
        return BAD_VALUE;

    tar = new ABuffer(OUTPUT_BUFFER_SIZE);
    if(tar == NULL)
        return BAD_VALUE;

    memset(tar->data(),0,OUTPUT_BUFFER_SIZE);

    tar_ptr = tar->data();
    src_ptr = src->data();
    header4 = fromSize * 8;

    memcpy(tar_ptr,&header1,sizeof(uint16_t));
    memcpy(tar_ptr+sizeof(uint16_t),&header2,sizeof(uint16_t));
    memcpy(tar_ptr+2*sizeof(uint16_t),&header3,sizeof(uint16_t));
    memcpy(tar_ptr+3*sizeof(uint16_t),&header4,sizeof(uint16_t));

    //need switch byte order for each uint16
    if(first == 0x0b){
        int32_t i;
        uint16_t * ptr = (uint16_t*)src_ptr;
        for(i = 0; i < (int32_t)fromSize/2; i++) {
        uint16_t word = ptr[i];
        uint16_t byte1 = (word << 8) & 0xff00;
        uint16_t byte2 = (word >> 8) & 0x00ff;
        ptr[i] = byte1 | byte2;
        }
    }
    if(fromSize < OUTPUT_BUFFER_SIZE){
        memcpy(tar_ptr+4*sizeof(uint16_t),src_ptr,fromSize);
    }

    ALOGV("parseAccessUnit 2,frameLen=%d,totalSrcSize=%d",fromSize,OUTPUT_BUFFER_SIZE);

    if(src->meta()->findInt64("timeUs", &timeUs)){
        tar->meta()->setInt64("timeUs",timeUs);
        ALOGV("parseAccessUnit ts=%lld",timeUs);
    }
    *accessUnit = tar;

    return OK;
}

status_t NuPlayer::DecoderPassThroughDDP::getCacheSize(size_t *cacheSize,size_t * bufferSize)
{
    if(cacheSize == NULL || bufferSize == NULL)
        return BAD_VALUE;
    *cacheSize = 10000;
    *bufferSize = 0;
    ALOGV("getCacheSize");
    return OK;
}
}

