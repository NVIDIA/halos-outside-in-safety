/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <memory>
#include <new>          /* std::nothrow, used by NvPSDCreateContext */

#include "NvPSD.h"
#include "NvPSD.hpp"
#include "pss_message_validate.h"


struct NvPSDCtx
{
    std::unique_ptr<nvpsd::NvPSD> mNvPSD;
};


/*
 * `new`, not `malloc`. NvPSDCtx holds a std::unique_ptr, and malloc does not
 * run its constructor -- so mNvPSD came back holding whatever was already in
 * that heap block.
 *
 * The first thing either initialize entry point does is
 *
 *     ctx->mNvPSD = std::make_unique<nvpsd::NvPSD>(...);
 *
 * and that assignment is a reset(): it DELETES the pointer already held before
 * taking the new one. With an uninitialised member that is `delete` on a wild
 * address, which lands in ~NvPSD() on garbage and takes the process with it.
 *
 * It was latent rather than harmless. A malloc that hands back a fresh, zeroed
 * page leaves mNvPSD null and `delete nullptr` is a no-op, so this survived
 * every path where the context was created early. nvpsd_gateway creates it
 * after NvPSB initialisation and argument parsing have already exercised the
 * heap, so it gets a recycled block, and the crash is 100% reproducible:
 *
 *     Program received signal SIGSEGV
 *     #0  nvpsd::NvPSD::~NvPSD()
 *     #1  std::__uniq_ptr_impl<nvpsd::NvPSD>::reset(nvpsd::NvPSD*)
 *     #2  NvPSDSocketInitialize()
 *     #3  main()
 *
 * The gateway does not start. Nothing reaches the SDMs and every one of them
 * latches on decision freshness.
 */
NvPSDCtx* NvPSDCreateContext()
{
    return new (std::nothrow) NvPSDCtx();
}


/*
 * NvPSDInitialize() has been removed.
 *
 * It was the POSIX_MSG_QUE entry point -- four message-queue names and a
 * hard-coded NvPSDChannelCreate(NVPSF_TRANSPORT_POSIX_MSG_QUE). Its only live
 * caller was the mqueue arm of SafetyEventManager; that arm was unreachable because the daemon defaults to
 * UNIX_SOCKET and rejects the config key at
 * startup.
 *
 * Use NvPSDSocketInitialize(ctx, endpoint) below. The channel names have no
 * successor: the endpoint role is the only parameter the socket link needs.
 */

NvPSDErr NvPSDSocketInitialize(NvPSDCtx* ctx, NvPSDEndpoint endpoint)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDSocketInitialize" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    ctx->mNvPSD = std::make_unique<nvpsd::NvPSD>(endpoint);
    err = ctx->mNvPSD->NvPSDChannelCreate(NVPSF_TRANSPORT_UNIX_SOCKET);
    if(err != NVPSD_SUCCESS)
    {
        err = NVPSD_FAIL;
        goto exit;
    }

exit:
    return err;
}

NvPSDErr NvPSDSetPssHeartbeatExternallyManaged(NvPSDCtx* ctx, int externallyManaged)
{
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDSetPssHeartbeatExternallyManaged" << std::endl;
        return NVPSD_FAIL;
    }
    if (!ctx->mNvPSD)
    {
        std::cerr << "ERROR: NvPSD context not initialized" << std::endl;
        return NVPSD_FAIL;
    }
    return ctx->mNvPSD->setPssHeartbeatExternallyManaged(externallyManaged != 0);
}

NvPSDErr NvPSDSetDecisionResponseTimeoutMs(NvPSDCtx* ctx, uint32_t timeoutMs)
{
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDSetDecisionResponseTimeoutMs" << std::endl;
        return NVPSD_FAIL;
    }
    if (!ctx->mNvPSD)
    {
        std::cerr << "ERROR: NvPSD context not initialized" << std::endl;
        return NVPSD_FAIL;
    }
    return ctx->mNvPSD->NvPSDSetDecisionResponseTimeoutMs(timeoutMs);
}

NvPSDErr NvPSDRegisterCallbacks(NvPSDCtx* ctx, NvPSDCallbacks* callbacks)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDRegisterCallbacks" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    // Validate callbacks pointer
    if (!callbacks)
    {
        std::cerr << "ERROR: NULL callbacks provided to NvPSDRegisterCallbacks" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    if(callbacks->processDecisionRequest == nullptr || callbacks->notifyShutdownRequest == nullptr)
    {
        std::cerr<<"processDecisionRequest and notifyShutdownRequest callbacks cannot be nullptr\n";
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDGetChannelEndpt() != NVPSD_CLIENT)
    {
        std::cerr<<"NvPSDRegisterCallbacks to be called by NVPSD_CLIENT only\n";
        err = NVPSD_FAIL;
        goto exit;
    }

    nvpsd::NvPSDCbInternal callbacksInternal;

    callbacksInternal.processDecisionRequest = callbacks->processDecisionRequest;
    callbacksInternal.publishDecisionResponse = callbacks->publishDecisionResponse;
    callbacksInternal.notifyShutdownRequest = callbacks->notifyShutdownRequest;

    ctx->mNvPSD->NvPSDSetCbs(callbacksInternal);

    ctx->mNvPSD->NvPSDChannelListenerStart();

exit:
    return err;
}

NvPSDErr NvPSDStart(NvPSDCtx* ctx)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDStart" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDGetChannelEndpt() != NVPSD_PSS)
    {
        std::cerr<<"NvPSDtart to be called by NVPSD_PSS only\n";
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDRequestStart() != NVPSD_SUCCESS)
    {
        err = NVPSD_FAIL;
    }

exit:
    return err;
}

NvPSDErr NvPSDProcessDecisionRequest(NvPSDCtx* ctx, const DecisionRequest* request, DecisionResponse* response)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDProcessDecisionRequest" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    // Validate request pointer
    if (!request)
    {
        std::cerr << "ERROR: NULL request provided to NvPSDProcessDecisionRequest" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    // Validate response pointer
    if (!response)
    {
        std::cerr << "ERROR: NULL response provided to NvPSDProcessDecisionRequest" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDGetChannelEndpt() != NVPSD_PSS)
    {
        std::cerr<<"NvPSDProcessDecisionRequest to be called by NVPSD_PSS only\n";
        err = NVPSD_FAIL;
        goto exit;
    }

    {
        uint32_t vErr = validateDecisionRequest(request);
        if (vErr != PSS_VALID)
        {
            std::cerr << "NvPSDProcessDecisionRequest: DecisionRequest validation failed (0x"
                      << std::hex << vErr << std::dec << ")" << std::endl;
            err = NVPSD_FAIL;
            goto exit;
        }
    }

    err = ctx->mNvPSD->NvPSDGenerateDecision(request, response);
    if(err != NVPSD_SUCCESS)
    {
        goto exit;
    }

exit:
    return err;
}

NvPSDErr NvPSDStop(NvPSDCtx* ctx)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDInitialize" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDGetChannelEndpt() != NVPSD_PSS)
    {
        std::cerr<<"NvPSDStop to be called by NVPSD_PSS only\n";
        err = NVPSD_FAIL;
        goto exit;
    }

    err = ctx->mNvPSD->NvPSDStop();
    if(err != NVPSD_SUCCESS)
    {
        err = NVPSD_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*
 * NvPSDExit() has been removed along with NvPSDInitialize -- it was the matching
 * NvPSDChannelClose(NVPSF_TRANSPORT_POSIX_MSG_QUE). Use NvPSDSocketExit().
 */

NvPSDErr NvPSDSocketExit(NvPSDCtx* ctx)
{
    NvPSDErr err = NVPSD_SUCCESS;

    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDInitialize" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    err = ctx->mNvPSD->NvPSDChannelClose(NVPSF_TRANSPORT_UNIX_SOCKET);
    if(err != NVPSD_SUCCESS)
    {
        err = NVPSD_FAIL;
        goto exit;
    }

exit:
    return err;
}

void NvPSDDestroyContext(NvPSDCtx* ctx)
{
    // Validate ctx pointer
    if (!ctx)
    {
        std::cerr << "ERROR: NULL context provided to NvPSDInitialize" << std::endl;
        return;
    }

    /* `delete`, matching the `new` in NvPSDCreateContext. free() would release
     * the block without running ~NvPSDCtx(), so the unique_ptr's destructor
     * would never run -- harmless only because of the explicit reset() above,
     * which is not a property worth depending on. */
    ctx->mNvPSD.reset();
    delete ctx;
}

NvPSDErr NvPSDRegisterEventTypes(NvPSDCtx* ctx, const EventType* eventTypes, uint32_t count)
{
    NvPSDErr err = NVPSD_SUCCESS;

    if(!ctx || !ctx->mNvPSD)
    {
        std::cerr << "Invalid NvPSD context" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    if(ctx->mNvPSD->NvPSDGetChannelEndpt() != NVPSD_CLIENT)
    {
        std::cerr << "NvPSDRegisterEventTypes to be called by NVPSD_CLIENT only" << std::endl;
        err = NVPSD_FAIL;
        goto exit;
    }

    err = ctx->mNvPSD->NvPSDRegisterEventTypes(eventTypes, count);
    if(err != NVPSD_SUCCESS)
    {
        err = NVPSD_FAIL;
        goto exit;
    }

exit:
    return err;
}
