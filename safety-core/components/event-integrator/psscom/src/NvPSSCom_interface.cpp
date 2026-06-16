/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <memory>
#include <iostream>

#include "NvPSSCom.h"
#include "NvPSSCom.hpp"
#include "posix_msg_que.h"

struct NvPSSComCtx
{
    std::unique_ptr<nvpss::NvPSSCom> mNvPSSCom;
};


NvPSSComCtx* NvPSSComCreateContext()
{
    NvPSSComCtx* ctx = new NvPSSComCtx();
    return ctx;
}

NvPSSComErr NvPSSComDataSrcInit(NvPSSComCtx* ctx, const char* writeChannel, const char* readChannel)
{

    NvPSSComErr err;
    ctx->mNvPSSCom = std::make_unique<nvpss::NvPSSCom>(writeChannel, readChannel, nvpss::NVPSSCOM_SRC);
    err = ctx->mNvPSSCom->NvPSSComChannelCreate(nvpss::POSIX_MSG_QUE);
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSSComErr NvPSSComDataSinkInit(NvPSSComCtx* ctx, const char* writeChannel, const char* readChannel)
{

    NvPSSComErr err;
    ctx->mNvPSSCom = std::make_unique<nvpss::NvPSSCom>(writeChannel, readChannel, nvpss::NVPSSCOM_SINK);
    err = ctx->mNvPSSCom->NvPSSComChannelCreate(nvpss::POSIX_MSG_QUE);
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    ctx->mNvPSSCom->NvPSSComChannelListenerStart();

exit:
    return err;
}

NvPSSComErr NvPSSDataSrcRegisterCallbacks(NvPSSComCtx* ctx, NvPSSComDataSrcCallbacks* srcCallbacks)
{
    nvpss::NvPSSComDataSrcCbInternal srcCallbacksInternal;

    srcCallbacksInternal.onDataRequest = srcCallbacks->onDataRequest;
    srcCallbacksInternal.onPause = srcCallbacks->onPause;
    srcCallbacksInternal.onResume = srcCallbacks->onResume;
    srcCallbacksInternal.onStop = srcCallbacks->onStop;

    ctx->mNvPSSCom->NvPSSComSetDataSrcCbs(srcCallbacksInternal);

    return NVPSSCOM_SUCCESS;
}

NvPSSComErr NvPSSDataSinkRegisterCallbacks(NvPSSComCtx* ctx, NvPSSComDataSinkCallbacks* sinkCallbacks)
{

    nvpss::NvPSSComDataSinkCbInternal sinkCallbacksInternal;

    sinkCallbacksInternal.onDataAvailable = sinkCallbacks->onDataAvailable;
    sinkCallbacksInternal.onFlowRateChange = sinkCallbacks->onFlowRateChange;
    sinkCallbacksInternal.onStop = sinkCallbacks->onStop;

    ctx->mNvPSSCom->NvPSSComSetDataSinkCbs(sinkCallbacksInternal);

    return NVPSSCOM_SUCCESS;
}

NvPSSComErr NvPSSComStart(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    if(ctx->mNvPSSCom->NvPSSComGetChannelEndpt() != nvpss::NVPSSCOM_SRC)
    {
        std::cerr<<"NvPSSStart to be called by NVPSSCOM_SRC only\n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    if(ctx->mNvPSSCom->NvPSSComRequestStart() != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
    }

    ctx->mNvPSSCom->NvPSSComChannelListenerStart();

exit:
    return err;
}

NvPSSComErr NvPSSComSetFlowRate(NvPSSComCtx* ctx ,uint8_t flowRate)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    if(ctx->mNvPSSCom->NvPSSComSetFlowRate(flowRate) != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }
exit:
    return err;
}

NvPSSComErr NvPSSComStop(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    err = ctx->mNvPSSCom->NvPSSComStop();
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSSComErr NvPSSComDataSrcExit(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    err = ctx->mNvPSSCom->NvPSSComChannelClose(nvpss::POSIX_MSG_QUE);
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSSComErr NvPSSComDataSinkExit(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    err = ctx->mNvPSSCom->NvPSSComChannelClose(nvpss::POSIX_MSG_QUE);
    if(err != NVPSSCOM_SUCCESS)
    {
        std::cerr<<"Error in terminating data sourece\n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSSComErr NvPSSComPushData(NvPSSComCtx* ctx, NvPSSComPacket* pkt)
{

    NvPSSComErr err = NVPSSCOM_SUCCESS;

    if(ctx->mNvPSSCom->NvPSSComGetChannelEndpt() != nvpss::NVPSSCOM_SRC)
    {
        std::cerr<<"NvPSSComPushData to be called by NVPSSCOM_SRC only\n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    err = ctx->mNvPSSCom->NvPSSComPushData(pkt);
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSSComErr NvPSSComPause(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    if(ctx->mNvPSSCom->NvPSSComGetChannelEndpt() != nvpss::NVPSSCOM_SINK)
    {
        std::cerr<<"NvPSSComPause to be called by NVPSSCOM_SINK only\n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    err = ctx->mNvPSSCom->NvPSSComPause();
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;

}

NvPSSComErr NvPSSComResume(NvPSSComCtx* ctx)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    if(ctx->mNvPSSCom->NvPSSComGetChannelEndpt() != nvpss::NVPSSCOM_SINK)
    {
        printf("NvPSSComResume to be called by NVPSSCOM_SINK only");
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    err = ctx->mNvPSSCom->NvPSSComResume();
    if(err != NVPSSCOM_SUCCESS)
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

void NvPSSComDestroyContext(NvPSSComCtx* ctx)
{
    if(ctx)
    {
        ctx->mNvPSSCom.reset();
        delete ctx;
    }
}
