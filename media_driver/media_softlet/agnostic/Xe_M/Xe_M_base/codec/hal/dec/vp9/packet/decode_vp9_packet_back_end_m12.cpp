/*
* Copyright (c) 2021, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     decode_vp9_packet_back_end_m12.cpp
//! \brief    Defines the interface for vp9 back end decode packet of M12
//!
#include "decode_vp9_packet_back_end_m12.h"
#include "decode_utils.h"
#include "decode_vp9_pipeline.h"
#include "decode_vp9_basic_feature.h"
#include "decode_status_report_defs.h"
#include "mos_solo_generic.h"
#include "decode_status_report_defs.h"
#include "decode_resource_auto_lock.h"
#include "hal_oca_interface.h"
#include "mhw_vdbox_vdenc_g12_X.h"
#include "decode_vp9_picture_packet_m12.h"

namespace decode
{

MOS_STATUS Vp9DecodeBackEndPktM12::Init()
{
    DECODE_FUNC_CALL();

    PERF_UTILITY_AUTO(__FUNCTION__, PERF_DECODE, PERF_LEVEL_HAL);

    DECODE_CHK_STATUS(Vp9DecodeBackEndPktXe_M_Base::Init());

    DecodeSubPacket* subPacket = m_vp9Pipeline->GetSubPacket(DecodePacketId(m_vp9Pipeline, vp9TileSubPacketId));
    m_tilePkt = dynamic_cast<Vp9DecodeTilePktM12*>(subPacket);
    DECODE_CHK_NULL(m_tilePkt);

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Vp9DecodeBackEndPktM12::Submit(
    MOS_COMMAND_BUFFER* cmdBuffer,
    uint8_t packetPhase)
{
    DECODE_FUNC_CALL();

    PERF_UTILITY_AUTO(__FUNCTION__, PERF_DECODE, PERF_LEVEL_HAL);

    DECODE_CHK_NULL(cmdBuffer);
    DECODE_CHK_NULL(m_hwInterface);

    DECODE_CHK_STATUS(m_picturePkt->SetPhase(m_phase));

    DECODE_CHK_STATUS(Mos_Solo_PreProcessDecode(m_osInterface, &m_vp9BasicFeature->m_destSurface));

    DECODE_CHK_STATUS(m_miInterface->SetWatchdogTimerThreshold(m_vp9BasicFeature->m_width, m_vp9BasicFeature->m_height, false));

    if (IsPrologRequired())
    {
        DECODE_CHK_STATUS(AddForceWakeup(*cmdBuffer));
        DECODE_CHK_STATUS(SendPrologWithFrameTracking(*cmdBuffer, true));
    }

    auto mmioRegisters = m_hwInterface->GetMfxInterface()->GetMmioRegisters(MHW_VDBOX_NODE_1);
    HalOcaInterface::On1stLevelBBStart(*cmdBuffer, *m_osInterface->pOsContext, m_osInterface->CurrentGpuContextHandle, *m_miInterface, *mmioRegisters);

    DECODE_CHK_STATUS(m_miInterface->AddWatchdogTimerStopCmd(cmdBuffer));
    auto scalability = m_vp9Pipeline->GetMediaScalability();
    DECODE_CHK_STATUS(scalability->SyncPipe(syncAllPipes, 0, cmdBuffer));

    Vp9DecodePicPktM12 *picturePkt = dynamic_cast<Vp9DecodePicPktM12 *>(m_picturePkt);
    DECODE_CHK_NULL(picturePkt);
    DECODE_CHK_STATUS(picturePkt->ValidateCabacStreamOutSize(*cmdBuffer));

    DECODE_CHK_STATUS(m_miInterface->AddWatchdogTimerStartCmd(cmdBuffer));
    DECODE_CHK_STATUS(PackPictureLevelCmds(*cmdBuffer));

    HalOcaInterface::On1stLevelBBEnd(*cmdBuffer, *m_osInterface);

    DECODE_CHK_STATUS(m_allocator->SyncOnResource(&m_vp9BasicFeature->m_resDataBuffer, false));

    DECODE_CHK_STATUS(Mos_Solo_PostProcessDecode(m_osInterface, &m_vp9BasicFeature->m_destSurface));

#if USE_CODECHAL_DEBUG_TOOL
    DECODE_CHK_STATUS(DumpSecondaryCommandBuffer(*cmdBuffer));
#endif

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Vp9DecodeBackEndPktM12::VdMemoryFlush(MOS_COMMAND_BUFFER &cmdBuffer)
{
    MHW_MI_VD_CONTROL_STATE_PARAMS vdCtrlParam;
    MOS_ZeroMemory(&vdCtrlParam, sizeof(MHW_MI_VD_CONTROL_STATE_PARAMS));
    vdCtrlParam.memoryImplicitFlush = true;

    MhwMiInterfaceG12* miInterfaceG12 = dynamic_cast<MhwMiInterfaceG12*>(m_miInterface);
    DECODE_CHK_NULL(miInterfaceG12);
    DECODE_CHK_STATUS(miInterfaceG12->AddMiVdControlStateCmd(&cmdBuffer, &vdCtrlParam));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Vp9DecodeBackEndPktM12::VdPipelineFlush(MOS_COMMAND_BUFFER & cmdBuffer)
{
    DECODE_FUNC_CALL();

    MHW_VDBOX_VD_PIPE_FLUSH_PARAMS_G12 vdpipeFlushParams;
    MOS_ZeroMemory(&vdpipeFlushParams, sizeof(vdpipeFlushParams));
    vdpipeFlushParams.Flags.bWaitDoneHEVC = 1;
    vdpipeFlushParams.Flags.bFlushHEVC = 1;
    vdpipeFlushParams.Flags.bWaitDoneVDCmdMsgParser = 1;
    DECODE_CHK_STATUS(m_vdencInterface->AddVdPipelineFlushCmd(
        &cmdBuffer, (MHW_VDBOX_VD_PIPE_FLUSH_PARAMS*)&vdpipeFlushParams));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Vp9DecodeBackEndPktM12::VdScalabPipeUnLock(MOS_COMMAND_BUFFER &cmdBuffer)
{
    MHW_MI_VD_CONTROL_STATE_PARAMS vdCtrlParam;
    MOS_ZeroMemory(&vdCtrlParam, sizeof(MHW_MI_VD_CONTROL_STATE_PARAMS));
    vdCtrlParam.scalableModePipeUnlock = true;

    MhwMiInterfaceG12* miInterfaceG12 = dynamic_cast<MhwMiInterfaceG12*>(m_miInterface);
    DECODE_CHK_NULL(miInterfaceG12);
    DECODE_CHK_STATUS(miInterfaceG12->AddMiVdControlStateCmd(&cmdBuffer, &vdCtrlParam));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Vp9DecodeBackEndPktM12::PackPictureLevelCmds(MOS_COMMAND_BUFFER &cmdBuffer)
{
    PERF_UTILITY_AUTO(__FUNCTION__, PERF_DECODE, PERF_LEVEL_HAL);

    DECODE_CHK_STATUS(StartStatusReport(statusReportMfx, &cmdBuffer));

    DECODE_CHK_STATUS(m_picturePkt->Execute(cmdBuffer));

    // add tile coding command
    DECODE_CHK_STATUS(m_tilePkt->Execute(cmdBuffer, m_vp9Pipeline->GetCurrentPipe()));

    DECODE_CHK_STATUS(VdMemoryFlush(cmdBuffer));
    DECODE_CHK_STATUS(VdScalabPipeUnLock(cmdBuffer));
    DECODE_CHK_STATUS(ReadVdboxId(cmdBuffer));
    DECODE_CHK_STATUS(VdPipelineFlush(cmdBuffer));
    DECODE_CHK_STATUS(MiFlush(cmdBuffer));

    auto scalability = m_vp9Pipeline->GetMediaScalability();
    DECODE_CHK_STATUS(scalability->SyncPipe(syncOnePipeWaitOthers, 0, &cmdBuffer));

    if (m_vp9Pipeline->IsFirstPipe())
    {
        DECODE_CHK_STATUS(EndStatusReport(statusReportMfx, &cmdBuffer));
        DECODE_CHK_STATUS(UpdateStatusReport(statusReportGlobalCount, &cmdBuffer));
    }
    else
    {
        DECODE_CHK_STATUS(NullHW::StopPredicate(m_miInterface, &cmdBuffer));
    }
    DECODE_CHK_STATUS(MiFlush(cmdBuffer));

    DECODE_CHK_STATUS(m_miInterface->AddMiBatchBufferEnd(&cmdBuffer, nullptr));

    return MOS_STATUS_SUCCESS;
}

}

