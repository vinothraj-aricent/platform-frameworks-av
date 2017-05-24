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
    mFrameSize = 0;
    ALOGW_IF(renderer == NULL, "expect a non-NULL renderer");
}
NuPlayer::DecoderPassThroughDDP::~DecoderPassThroughDDP()
{
    if(tempBuf != NULL){
        tempBuf.clear();
        tempBuf = NULL;
    }
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
    int32_t src_offset = 0;
    int32_t copy_offset = 0;
    uint32_t fromSize = src->size();
    uint8_t* src_ptr= NULL;
    uint8_t* tar_ptr= NULL;
    int32_t frameCnt = 0;
    uint32_t blockNum = 0;

    //skip empty buffer
    if(fromSize == 0)
        return OK;

    ALOGV("parseAccessUnit from size=%d",fromSize);

    if(tempBuf != NULL){
        uint8_t* temp_in_ptr= NULL;
        int32_t lastBuf_offset = tempBuf->offset();
        int32_t tempBufSize = tempBuf->size();
        temp_in_ptr = src->data();
        src = new ABuffer(tempBufSize + fromSize);
        ALOGV("last buffer offset=%d,size=%d",lastBuf_offset,tempBufSize);
        memcpy(src->data(),tempBuf->data(),tempBufSize);
        memcpy(src->data()+tempBufSize,temp_in_ptr,fromSize);
        fromSize = src->size();
        ALOGV("copy left buffer size=%d,new buffer size=%d",tempBufSize,fromSize);
        if((*accessUnit)->meta()->findInt64("timeUs", &timeUs)){
            src->meta()->setInt64("timeUs",timeUs);
        }
        (*accessUnit).clear();
        tempBuf.clear();
        tempBuf = NULL;
    }

    while(src_offset < (int32_t)fromSize){
        int32_t frameLen = 0;
        if(OK != getFrameLen(&src,src_offset,&frameLen)){
            src_offset += 1;
            ALOGE("can't find DDP header");
            continue;
        }else{
            ALOGV("src_offset=%d,frameLen=%d",src_offset,frameLen);
        }
        if(src_offset + frameLen > (int32_t)fromSize){
            ALOGV("src_offset=%d,frameLen=%d,size=%d",src_offset,frameLen,fromSize);
            break;
        }
        src_offset += frameLen;
        frameCnt++;
        blockNum += mNumBlocks;
        if(blockNum == 6)
            break;
    }

    //wait until get 6 audio blocks
    if(blockNum < 6){
        ALOGV("blockNum =%d, wait another frame",blockNum);
        if(tempBuf != NULL)
            tempBuf.clear();
        tempBuf = src;
        return -EWOULDBLOCK;
    }

    uint8_t first = *src->data();

    #define OUTPUT_BUFFER_SIZE (6144*4)
    tar = new ABuffer(OUTPUT_BUFFER_SIZE);
    if(tar == NULL)
        return BAD_VALUE;

    memset(tar->data(),0,OUTPUT_BUFFER_SIZE);

    copy_offset = 0;

    {
        tar_ptr = tar->data();
        src_ptr = src->data();
        header4 = src_offset * 8;

        memcpy(tar_ptr,&header1,sizeof(uint16_t));
        memcpy(tar_ptr+sizeof(uint16_t),&header2,sizeof(uint16_t));
        memcpy(tar_ptr+2*sizeof(uint16_t),&header3,sizeof(uint16_t));
        memcpy(tar_ptr+3*sizeof(uint16_t),&header4,sizeof(uint16_t));

        //need switch byte order for each uint16
        if(first == 0x0b){
            int32_t i;
            uint16_t * ptr = (uint16_t*)src_ptr;
            for(i = 0; i < (int32_t)src_offset/2; i++) {
            uint16_t word = ptr[i];
            uint16_t byte1 = (word << 8) & 0xff00;
            uint16_t byte2 = (word >> 8) & 0x00ff;
            ptr[i] = byte1 | byte2;
            }
        }

        if(src_offset+8 < OUTPUT_BUFFER_SIZE){
            memcpy(tar_ptr+4*sizeof(uint16_t),src_ptr,src_offset);
        }
    }

    ALOGV("parseAccessUnit 2,totalSrcSize=%d",fromSize);

    if(src->meta()->findInt64("timeUs", &timeUs)){
        tar->meta()->setInt64("timeUs",timeUs);
        ALOGV("parseAccessUnit ts=%lld",timeUs);
    }
    *accessUnit = tar;

    return OK;
}
status_t NuPlayer::DecoderPassThroughDDP::getFrameLen(sp<ABuffer> *accessUnit,int32_t offset,int32_t *len)
{
    int32_t fscod = 0;
    int32_t frmsizecod = 0;
    int fscod2 = -1;
    mNumBlocks = 0;
    uint8_t * pHeader = NULL;

    if(accessUnit == NULL || len == NULL)
        return BAD_VALUE;

    sp<ABuffer> from = *accessUnit;
    if(from->size() < 8 || offset + 8 > (int32_t)from->size())
        return BAD_VALUE;
    pHeader = from->data()+offset;
    if ((pHeader[0] == 0x0b && pHeader[1] == 0x77) || (pHeader[0] == 0x77 && pHeader[1] == 0x0b)){
        ;
    }else{
        return BAD_VALUE;
    }
    if(pHeader[0] == 0x0b && pHeader[1] == 0x77){
        frmsizecod = (((pHeader[2] & 0x7) << 8) + pHeader[3] + 1)*2;
        fscod = pHeader[4] >> 6;
        if(fscod == 0x3){
            mNumBlocks = 6;
            fscod2 = (pHeader[4] & 0x3f) >> 4;
        }else
            mNumBlocks = (pHeader[4] & 0x3f) >> 4;
    }else if(pHeader[0] == 0x77 && pHeader[1] == 0x0b){

        fscod = pHeader[5] >> 6;
        frmsizecod = (((pHeader[3] & 0x7) << 8) + pHeader[2] + 1)*2;
        if(fscod == 0x3){
            mNumBlocks = 6;
            fscod2 = (pHeader[5] & 0x3f) >> 4;
        }else
            mNumBlocks = (pHeader[5] & 0x3f) >> 4;
    }
    ALOGV("fscod2 =%d",fscod2);
    if(3 == mNumBlocks)
        mNumBlocks = 6;
    else if(mNumBlocks < 3)
        mNumBlocks += 1;

    ALOGV("numBlocks = %d fscod = %d size=%d",mNumBlocks,fscod,frmsizecod);
    *len = frmsizecod;
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

