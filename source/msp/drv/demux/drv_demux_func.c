/*********************************************************************************
*
*  Copyright (C) 2014 Hisilicon Technologies Co., Ltd.  All rights reserved. 
*
*  This program is confidential and proprietary to Hisilicon Technologies Co., Ltd.
*  (Hisilicon), and may not be copied, reproduced, modified, disclosed to
*  others, published or used, in whole or in part, without the express prior
*  written permission of Hisilicon.
*
***********************************************************************************/

#include <linux/kernel.h>

#include "hi_type.h"
#include "hi_module.h"
#include "hi_drv_mmz.h"
#include "hi_drv_mem.h"
#include "drv_sys_ext.h"
#include "hi_kernel_adapt.h"

#include "drv_demux_define.h"
#include "demux_debug.h"
#include "hal_demux.h"
#include "drv_demux_reg.h"
#include "drv_demux_func.h"
#include "drv_demux_scd.h"
#include "drv_demux_index.h"
#include "drv_demux_sw.h"
#include "drv_demux_osal.h"
#include "drv_demux_ioctl.h"

#ifdef DMX_DESCRAMBLER_SUPPORT
#include "drv_desc_ext.h"
#endif

#include "hi_drv_module.h"
#include "drv_vdec_ext.h"
#include "drv_sync_ext.h"

#define DMX_GET_REGION_ID(DmxId)            ((0 == (DmxId)) ? 0 : ((4 == (DmxId)) ? 2 : 1))

#define DMX_PES_CHANNEL_MIN_SIZE            0x10000

#define DMX_RAM_PORT_AUTO_REGION            60

#define DMX_RAM_PORT_AUTO_STEP_1            0
#define DMX_RAM_PORT_AUTO_STEP_2            1
#define DMX_RAM_PORT_AUTO_STEP_4            2
#define DMX_RAM_PORT_AUTO_STEP_8            3

#define DMX_FQ_VID_BLOCK_SIZE               8192
#define DMX_FQ_AUD_BLOCK_SIZE               1280

#define DMX_REC_MIN_BUF_SIZE                (256 * 1024)

#define DMX_REC_VID_SCD_BUF_SIZE            (56 * 1024)
#define DMX_REC_AUD_SCD_BUF_SIZE            (28 * 1024)

#define DMX_REC_TS_PACKETS_UNIT             (4 * 188)


#define DMX_REC_VID_TS_PACKETS_PER_BLOCK    (64 * DMX_REC_TS_PACKETS_UNIT)
#define DMX_REC_AUD_TS_PACKETS_PER_BLOCK    (32 * DMX_REC_TS_PACKETS_UNIT)

#define DMX_REC_TS_WITH_TIMESTAMP_UNIT      (4 * 192)
#define DMX_REC_VID_TS_WITH_TIMESTAMP_BLOCK_SIZE    (64 * DMX_REC_TS_WITH_TIMESTAMP_UNIT)
#define DMX_REC_AUD_TS_WITH_TIMESTAMP_BLOCK_SIZE    (32 * DMX_REC_TS_WITH_TIMESTAMP_UNIT)

#ifndef HI_DEMUX_SCD_SUPPORT
#define DMX_REC_SOFT_INDEX_TIME_GAP         (100)    /*for soft index, the min_time gap is 100ms*/
#endif

#define DMX_REC_VID_SCD_NUM_PER_BLOCK       (8 * 28)
/*
fqdepth == DMX_REC_AUD_SCD_BUF_SIZE/DMX_REC_AUD_SCD_PER_BLOCK  >= 1023 , so ,the min DMX_REC_AUD_SCD_PER_BLOCK should be 28
*/
#define DMX_REC_AUD_SCD_NUM_PER_BLOCK       (4 * 28)

#define DMX_REC_SCD_INVALID_CHANNEL         0x7F

#define DMX_MAX_SCR_MS                      (47721858)  // scd wrap around value in ms: 0x100000000 / 90

#define DMX_FQ_DESC_SIZE                    0x8
#define DMX_OQ_DESC_SIZE                    0x10

#define MIN_MMZ_BUF_SIZE                    4096

#define DMX_TS_BUFFER_EMPTY                 2048

DMX_DEV_OSI_S *g_pDmxDevOsi = HI_NULL;

SYNC_EXPORT_FUNC_S      *g_pSyncFunc    = HI_NULL;

#ifdef DMX_DESCRAMBLER_SUPPORT
DSC_RegisterFunctionlist_S      *g_pDscFunc    = HI_NULL;
#endif


#ifdef CHIP_TYPE_hi3716m
HI_BOOL Hi3716MV300Flag = HI_FALSE;
#endif

static HI_U32 s_u32DMXCRCErr[DMX_CHANNEL_CNT] = {0};
static HI_U32 s_u32DMXTEIErr[DMX_CHANNEL_CNT] = {0};
static HI_U32 s_u32DMXCCDiscErr[DMX_CHANNEL_CNT] = {0};
static HI_U32 s_u32DMXPesLenErr[DMX_CHANNEL_CNT] = {0};
HI_U32 s_u32DMXDropCnt[DMX_CHANNEL_CNT] = {0};

static HI_U32 s_u32DMXChanDatafl[DMX_CHANNEL_CNT] = {0};
static HI_U32 s_u32DMXBufEop[DMX_OQ_CNT] = {0};
static HI_U32 s_u32DMXBufData = 0;

static DMX_ERRMSG_S psErrMsg[DMX_MAX_ERRLIST_NUM];
static wait_queue_head_t AllChnWaitQueue;
static wait_queue_head_t *s_pAllChnWatchWaitQueue = HI_NULL;


#define IsDmxDevInit()                                      \
    do                                                      \
    {                                                       \
        if (!g_pDmxDevOsi)                                  \
        {                                                   \
            HI_FATAL_DEMUX("global is not exist!\n");       \
            return HI_ERR_DMX_NOT_INIT;                     \
        }                                                   \
    } while (0)

extern HI_VOID DMX_OsrSaveEs(HI_U32 type, HI_U8 *buf, HI_U32 len,HI_U32 chnid);
static HI_VOID DMXOsiOQGetReadWrite(HI_U32 OQId, HI_U32 *BlockWrite, HI_U32 *BlockRead);

HI_VOID CheckChnTimeoutProc(HI_LENGTH_T TimerPara);
DEFINE_TIMER(CheckChnTimeoutTimer, CheckChnTimeoutProc, 0, 0);

static HI_U32 GetQueueLenth(const HI_U32 Read, const HI_U32 Write, const HI_U32 Size)
{
    HI_U32 ret;

    if (Read > Write)
    {
        ret = Size + Write - Read;
    }
    else
    {
        ret = Write - Read;
    }

    return ret;
}



static HI_VOID DMXCheckFQBPStatus(DMX_Sub_DevInfo_S *DmxInfo, HI_U32 FQId)
{
    if (DMX_PORT_MODE_RAM == DmxInfo->PortMode)
    {
        HI_U32 FQ_WPtr, FQ_RPtr, ValidDes, tmp;
        DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;

        if (DmxHalGetIPBPStatus(DmxInfo->PortId))
        {
            DmxHalGetFQWORDx(FQId, DMX_FQ_SZWR_OFFSET, &tmp);
            FQ_WPtr = tmp & 0xffff;
            DmxHalGetFQWORDx(FQId, DMX_FQ_RDVD_OFFSET, &tmp);
            FQ_RPtr = tmp & 0xffff;

            if (FQ_WPtr >= FQ_RPtr)
            {
                ValidDes = FQ_WPtr - FQ_RPtr;
            }
            else
            {
                ValidDes = (pDmxDevOsi->DmxFqInfo[FQId].u32FQDepth) - FQ_RPtr + FQ_WPtr;
            }

            if (ValidDes >= (pDmxDevOsi->DmxFqInfo[FQId].u32FQDepth / 10))
            {
                DmxHalClrIPFqBPStatus(DmxInfo->PortId, FQId);
            }
        }
    }
}

static HI_S32 DmxFlushChannel(HI_U32 ChanId, DMX_FLUSH_TYPE_E FlushType)
{
    HI_S32  ret = HI_SUCCESS;
    HI_U32  i;

    DmxHalFlushChannel(ChanId, FlushType);

    for (i = 0; i < DMX_MAX_FLUSH_WAIT; i++)
    {
        if (DmxHalIsFlushChannelDone())
        {
            break;
        }
    }

    if (DMX_MAX_FLUSH_WAIT == i)
    {
        HI_ERR_DEMUX("flush channel %u failed\n", ChanId);

        ret = HI_FAILURE;
    }

    return ret;
}

static HI_S32 DmxFlushOq(HI_U32 OqId, DMX_OQ_CLEAR_TYPE_E ClearType)
{
    HI_S32  ret = HI_SUCCESS;
    HI_U32  i;

    DmxHalClearOq(OqId, ClearType);

    for (i = 0; i < DMX_MAX_FLUSH_WAIT; i++)
    {
        if (DmxHalIsClearOqDone())
        {
            break;
        }
    }

    if (DMX_MAX_FLUSH_WAIT == i)
    {
        HI_ERR_DEMUX("clear oq %u failed\n", OqId);

        ret = HI_FAILURE;
    }

    return ret;
}

//enable oq recive
//close channel dmx ->  disable oq -> flush dmx channel
//-> clear oq reg infor -> enable oq -> set channel dmx
static HI_S32 DMXOsiEnableOQRecive(HI_U32 ChanId, HI_U32 OqId)
{
    HI_U32  DmxId;
    HI_U32  Word;

    DmxHalGetChannelPlayDmxid(ChanId, &DmxId);
    DmxHalSetChannelPlayDmxid(ChanId, DMX_INVALID_DEMUX_ID);
    DmxHalDisableOQRecive(OqId);

    DmxFlushChannel(ChanId, DMX_FLUSH_TYPE_REC_PLAY);

    DmxHalSetOQWORDx(OqId, DMX_OQ_EOPWR_OFFSET, 0 );
    DmxHalGetOQWORDx(OqId, DMX_OQ_RSV_OFFSET, &Word);
    Word &= 0xffffff00;
    DmxHalSetOQWORDx(OqId, DMX_OQ_RSV_OFFSET, Word);
    DmxHalGetOQWORDx(OqId, DMX_OQ_CTRL_OFFSET, &Word);
    Word &= 0x80;
    DmxHalSetOQWORDx(OqId, DMX_OQ_CTRL_OFFSET, Word);

    DmxHalEnableOQRecive(OqId);
    DmxHalSetChannelPlayDmxid(ChanId, DmxId);

    return 0;
}

static HI_S32 ResetOQ(HI_U32 u32DmxId, DMX_OQ_Info_S *pstOQInfo)
{
    HI_U32 i, u32PvrCtrl;
    HI_U32 u32WriteBlk, u32OqRead, u32OqWrite;
    HI_U32 u32CurrentBlk, u32BufPhyAddr, u32BlkSize, u32BlkNum;
    HI_S32 s32OQEnableStatus;
    FQ_DescInfo_S* FQ_Desc;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo;
    HI_SIZE_T u32LockFlag;

    IsDmxDevInit();

    s32OQEnableStatus = DmxHalGetOQEnableStatus(pstOQInfo->u32OQId);
    DmxHalDisableOQRecive(pstOQInfo->u32OQId);
    DMXOsiOQGetReadWrite(pstOQInfo->u32OQId, &u32OqWrite, &u32OqRead);

    if (!pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId].u32FQVirAddr)
    {
        HI_ERR_DEMUX("fq not valid \n");

        DMXOsiEnableOQRecive(pstOQInfo->u32AttachId,pstOQInfo->u32OQId);
        return HI_FAILURE;
    }

    FqInfo = &pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId];
    
    spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);  

    u32WriteBlk = DmxHalGetFQWritePtr(pstOQInfo->u32FQId);

    u32BlkNum = GetQueueLenth(u32OqRead, u32OqWrite, pstOQInfo->u32OQDepth);
    for (i = 0; i < u32BlkNum; i++)
    {
        u32CurrentBlk = pstOQInfo->u32OQVirAddr + u32OqRead * DMX_OQ_DESC_SIZE;
        u32BufPhyAddr = *(HI_U32 *)u32CurrentBlk;
        u32BlkSize = *(HI_U32 *)(u32CurrentBlk + 4) & 0xffff;
        u32CurrentBlk = pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId].u32FQVirAddr
                        + u32WriteBlk * DMX_FQ_DESC_SIZE;
        FQ_Desc = (FQ_DescInfo_S*)u32CurrentBlk;
        FQ_Desc->start_addr = u32BufPhyAddr;
        FQ_Desc->buflen = u32BlkSize;
        
        BUG_ON(!(FQ_Desc->start_addr >= FqInfo->u32BufPhyAddr && FQ_Desc->start_addr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize));
        
        DMXINC(u32WriteBlk, pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId].u32FQDepth);
        DMXINC(u32OqRead, pstOQInfo->u32OQDepth);
    }

    DmxHalGetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_CTRL_OFFSET, &u32PvrCtrl);
    if (u32PvrCtrl & DMX_MASK_BIT_7)
    {
        DmxHalGetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_SADDR_OFFSET, &u32BufPhyAddr);
        DmxHalGetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_SZUS_OFFSET, &u32BlkSize);
        u32BlkSize  >>= 16;
        u32CurrentBlk = pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId].u32FQVirAddr
                        + u32WriteBlk * DMX_FQ_DESC_SIZE;
        FQ_Desc = (FQ_DescInfo_S*)u32CurrentBlk;
        FQ_Desc->start_addr = u32BufPhyAddr;
        FQ_Desc->buflen = u32BlkSize;

        BUG_ON(!(FQ_Desc->start_addr >= FqInfo->u32BufPhyAddr && FQ_Desc->start_addr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize));

        DMXINC(u32WriteBlk, pDmxDevOsi->DmxFqInfo[pstOQInfo->u32FQId].u32FQDepth);
    }

    DmxHalSetFQWritePtr(pstOQInfo->u32FQId, u32WriteBlk);

    DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[u32DmxId], pstOQInfo->u32FQId); //CLR BP STATUS

    spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);  

    pstOQInfo->u32ProcsBlk = 0;
    pstOQInfo->u32ProcsOffset = 0;
    pstOQInfo->u32ReleaseBlk = 0;
    pstOQInfo->u32ReleaseOffset = 0;

    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_RSV_OFFSET, 0);
    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_CTRL_OFFSET, 0);
    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_EOPWR_OFFSET, 0 );
    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_SZUS_OFFSET, 0);
    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_SADDR_OFFSET, 0);
    DmxHalSetOQWORDx(pstOQInfo->u32OQId, DMX_OQ_RDWR_OFFSET, 0);

    //reset oq ts counter
    DmxHalResetOqCounter(pstOQInfo->u32OQId);
    if (s32OQEnableStatus)
    {
        DMXOsiEnableOQRecive(pstOQInfo->u32AttachId,pstOQInfo->u32OQId);
    }

    return HI_SUCCESS;
}

static HI_VOID TsBufferConfig(const HI_U32 PortId, const HI_BOOL Enable, const HI_U32 DescPhyAddr, const HI_U32 DescDepth)
{
    DmxHalIPPortDescSet(PortId, DescPhyAddr, DescDepth);

    DmxHalIPPortSetOutInt(PortId, Enable);

    DmxHalIPPortStartStream(PortId, Enable);
}

static HI_U32 DmxFqIdAcquire(HI_U32 MinFqId, HI_U32 MaxFqId)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = DmxMgr->DmxFqInfo;
    HI_U32          FqId    = DMX_INVALID_FQ_ID;
    HI_U32          i;
    HI_SIZE_T       u32LockFlag;

    for (i = MinFqId; i < MaxFqId && i < DMX_FQ_CNT; i++)
    {
        spin_lock_irqsave(&FqInfo[i].LockFq, u32LockFlag);
        
        if (0 == FqInfo[i].u32IsUsed)
        {
            FqInfo[i].u32IsUsed = 1;

            FqId = i;
        }

        spin_unlock_irqrestore(&FqInfo[i].LockFq, u32LockFlag);

        if (DMX_INVALID_FQ_ID != FqId)
        {
            break;
        }
    }

    return FqId;
}

static HI_VOID DmxFqIdRelease(HI_U32 FqId)
{
    HI_SIZE_T       u32LockFlag;
    
    if (FqId < DMX_FQ_CNT)
    {
        DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
        DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[FqId];

        spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);
        
        FqInfo->u32IsUsed = 0;

        spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);
    }
}

static HI_VOID DmxFqStart(HI_U32 FqId)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[FqId];
    FQ_DescInfo_S  *FqDesc  = (FQ_DescInfo_S*)FqInfo->u32FQVirAddr;
    HI_U32          PhyAddr = FqInfo->u32BufPhyAddr;
    HI_U32          i;

    for (i = 0; i < FqInfo->u32FQDepth - 1; i++)
    {
        FqDesc->start_addr  = PhyAddr;
        FqDesc->buflen      = FqInfo->u32BlockSize;

        PhyAddr += (FqInfo->u32BlockSize + DMX_BUF_INTERVAL);

        ++FqDesc;
    }

    FqDesc->start_addr  = 0;
    FqDesc->buflen      = 0;

    DmxHalSetFQWORDx(FqId, DMX_FQ_CTRL_OFFSET, DMX_FQ_ALOVF_CNT << 24);
    DmxHalSetFQWORDx(FqId, DMX_FQ_RDVD_OFFSET, 0);
    DmxHalSetFQWORDx(FqId, DMX_FQ_SZWR_OFFSET, (FqInfo->u32FQDepth << 16) | ((FqInfo->u32FQDepth - 1) & 0xffff));
    DmxHalSetFQWORDx(FqId, DMX_FQ_START_OFFSET, FqInfo->u32FQPhyAddr);

    FqInfo->FqOverflowCount = 0;
    DmxHalFQClearOverflowInt(FqId);
    DmxHalFQSetOverflowInt(FqId, DMX_ENABLE);

    DmxHalFQEnableRecive(FqId, DMX_ENABLE);
}

static HI_VOID DmxFqStop(HI_U32 FqId)
{
    DmxHalFQEnableRecive(FqId, DMX_DISABLE);

    DmxHalFQSetOverflowInt(FqId, DMX_DISABLE);
    DmxHalFQClearOverflowInt(FqId);

    DmxHalSetFQWORDx(FqId, DMX_FQ_CTRL_OFFSET, 0);
    DmxHalSetFQWORDx(FqId, DMX_FQ_RDVD_OFFSET, 0);
    DmxHalSetFQWORDx(FqId, DMX_FQ_SZWR_OFFSET, 0);
    DmxHalSetFQWORDx(FqId, DMX_FQ_START_OFFSET, 0);
}

static HI_VOID DmxFqCheckOverflowInt(HI_U32 FqId)
{
    if (!DmxHalFQIsEnableOverflowInt(FqId))
    {
        DmxHalFQSetOverflowInt(FqId, DMX_ENABLE);
    }
}

static HI_VOID DmxOqStart(HI_U32 OqId)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[OqId];

    memset((HI_VOID*)OqInfo->u32OQVirAddr, 0, OqInfo->u32OQDepth * DMX_OQ_DESC_SIZE);

    OqInfo->u32ProcsBlk         = 0;
    OqInfo->u32ProcsOffset      = 0;
    OqInfo->u32ReleaseBlk       = 0;
    OqInfo->u32ReleaseOffset    = 0;

    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_RSV_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_CTRL_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_EOPWR_OFFSET, 0 );
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_SZUS_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_SADDR_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_RDWR_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_CFG_OFFSET, DMX_OQ_OUTINT_CNT << 26 |
                     OqInfo->u32OQDepth << 16 | (DMX_OQ_ALOVF_CNT & 0xff) << 8 | OqInfo->u32FQId);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_START_OFFSET, OqInfo->u32OQPhyAddr);

    DmxHalResetOqCounter(OqInfo->u32OQId);

    DmxHalEnableOQRecive(OqInfo->u32OQId);
}

static HI_VOID DmxOqStop(HI_U32 OqId)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[OqId];

    DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_FALSE);

    DmxHalDisableOQRecive(OqInfo->u32OQId);

    DmxHalResetOqCounter(OqInfo->u32OQId);

    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_RSV_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_CTRL_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_EOPWR_OFFSET, 0 );
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_SZUS_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_SADDR_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_RDWR_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_CFG_OFFSET, 0);
    DmxHalSetOQWORDx(OqInfo->u32OQId, DMX_OQ_START_OFFSET, 0);
}

static HI_S32 DmxOQRelease(HI_U32 FqId, HI_U32 OqId, HI_U32 PhyAddr, HI_U32 len)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[FqId];
    DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[OqId];
    OQ_DescInfo_S  *OqDesc;
    FQ_DescInfo_S  *FqDesc;
    HI_U32          OqRead;
    HI_U32          FqWrite;
    HI_SIZE_T       u32LockFlag;

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, HI_NULL, &OqRead);

    OqDesc = (OQ_DescInfo_S*)(OqInfo->u32OQVirAddr + OqRead * DMX_OQ_DESC_SIZE);

    if (!(OqDesc->start_addr >= FqInfo->u32BufPhyAddr && OqDesc->start_addr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize))
    {
        HI_ERR_DEMUX("Invalid OQ(%d) descriptor[0x%x, 0x%x].\n", OqInfo->u32OQId, OqDesc->start_addr, OqDesc->cactrl_buflen & 0xffff);
        
        return HI_ERR_DMX_INVALID_PARA;
    }

    if ((DMX_OQ_MODE_REC == OqInfo->enOQBufMode) || (DMX_OQ_MODE_SCD == OqInfo->enOQBufMode))
    {
        HI_U32 DataLen = OqDesc->pvrctrl_datalen & 0xffff;

        if ((PhyAddr != OqDesc->start_addr) || (DataLen != len))
        {
            HI_ERR_DEMUX("buf %d release wrong data block.\n", OqInfo->u32OQId);

            return HI_ERR_DMX_INVALID_PARA;
        }
        /*Delete the following code, because:
        Customers demands that rec data can be acquire several times then release them one by one.
        so we changed the DMX_DRV_REC_AcquireRecData and use the OqInfo->u32ProcsBlk to judge wether there 
        have data in Rec buf. and ,every time after acquired a block of rec data (47K),
        the OqInfo->u32ProcsBlk++ , so .at here ,it can not  be added again*/
        DMXINC(OqInfo->u32ProcsBlk, OqInfo->u32OQDepth);
        OqInfo->u32ProcsOffset = 0;
    }   

    DMXINC(OqRead, OqInfo->u32OQDepth);

    DmxHalSetOQReadPtr(OqInfo->u32OQId, OqRead);

    spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);
    
    FqWrite = DmxHalGetFQWritePtr(FqId);

    FqDesc = (FQ_DescInfo_S*)(FqInfo->u32FQVirAddr + FqWrite * DMX_FQ_DESC_SIZE);

    FqDesc->start_addr  = OqDesc->start_addr;
    FqDesc->buflen      = OqDesc->cactrl_buflen & 0xffff;

    DMXINC(FqWrite, FqInfo->u32FQDepth);

    DmxHalSetFQWritePtr(FqId, FqWrite);

    spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);

    DmxFqCheckOverflowInt(FqId);

    return HI_SUCCESS;
}

#ifdef HI_DEMUX_REC_SUPPORT
static HI_S32 DmxOQReleaseByBlockCnt(HI_U32 FqId, HI_U32 OqId, HI_U32 BlockCnt)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[FqId];
    DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[OqId];
    OQ_DescInfo_S  *OqDesc;
    FQ_DescInfo_S  *FqDesc;
    HI_U32          OqRead;
    HI_U32          FqWrite;
    HI_SIZE_T       u32LockFlag;
    HI_U32 i = 0;

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, HI_NULL, &OqRead);
    FqWrite = DmxHalGetFQWritePtr(FqId);

    for ( i = 0 ; i < BlockCnt ; i++ )
    {
        OqDesc = (OQ_DescInfo_S*)(OqInfo->u32OQVirAddr + OqRead * DMX_OQ_DESC_SIZE);
        FqDesc = (FQ_DescInfo_S*)(FqInfo->u32FQVirAddr + FqWrite * DMX_FQ_DESC_SIZE);
        FqDesc->start_addr  = OqDesc->start_addr;
        FqDesc->buflen      = OqDesc->cactrl_buflen & 0xffff;

        BUG_ON(!(FqDesc->start_addr >= FqInfo->u32BufPhyAddr && FqDesc->start_addr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize));
        
        DMXINC(OqRead, OqInfo->u32OQDepth);
        DMXINC(FqWrite, FqInfo->u32FQDepth);
    }

    DmxHalSetOQReadPtr(OqInfo->u32OQId, OqRead);
    spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);
    DmxHalSetFQWritePtr(FqId, FqWrite);
    spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);

    DmxFqCheckOverflowInt(FqId);

    return HI_SUCCESS;
}

static HI_VOID DmxRecFlush(HI_U32 RecId)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    HI_U32          DmxId   = RecInfo->DmxId;
    HI_U32          i;

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &DmxMgr->DmxChanInfo[i];

        if (ChanInfo->DmxId == DmxId)
        {
            if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC & ChanInfo->ChanOutMode)
            {
                DmxFlushChannel(ChanInfo->ChanId, DMX_FLUSH_TYPE_REC);
            }
        }
    }

    DmxFlushOq(RecInfo->RecOqId, DMX_OQ_CLEAR_TYPE_REC);
#ifdef HI_DEMUX_SCD_SUPPORT
    if (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType)
    {
        DmxHalFlushScdBuf(RecInfo->ScdId);
        DmxHalClrScdCnt(RecInfo->ScdId);
        DmxFlushOq(RecInfo->ScdOqId, DMX_OQ_CLEAR_TYPE_SCD);
    }
#endif
}
#endif

static HI_VOID DmxSetChannel(HI_U32 ChanId)
{
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
        DmxHalSetChannelPlayBufId(ChanId, ChanInfo->ChanOqId);
    }

    if ((HI_UNF_DMX_CHAN_TYPE_SEC == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_ECM_EMM == ChanInfo->ChanType))
    {
        DmxHalSetChannelDataType(ChanId, DMX_CHAN_DATA_TYPE_SEC);
    }
    else
    {
        DmxHalSetChannelDataType(ChanId, DMX_CHAN_DATA_TYPE_PES);
    }

    DmxHalSetChannelPusiCtrl(ChanId, DMX_DISABLE);

    if (HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType)
    {
        DmxHalSetChannelAttr(ChanId, DMX_CH_AUDIO);
    }
    else  if (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType)
    {
        DmxHalSetChannelAttr(ChanId, DMX_CH_VIDEO);
    }
    else if (HI_UNF_DMX_CHAN_TYPE_PES == ChanInfo->ChanType)
    {
        DmxHalSetChannelAttr(ChanId, DMX_CH_PES);
    }
    else
    {
        DmxHalSetChannelAttr(ChanId, DMX_CH_GENERAL);
        if (HI_UNF_DMX_CHAN_TYPE_POST == ChanInfo->ChanType)
        {
            DmxHalSetChannelTsPostMode(ChanId, DMX_ENABLE);
            DmxHalSetChannelTsPostThresh(ChanId, DMX_DEFAULT_POST_TH);
            DmxHalSetChannelPusiCtrl(ChanId, DMX_ENABLE);    // receive the ts packet do not have pusi
        }
    }

    DmxHalSetChannelCRCMode(ChanId, ChanInfo->ChanCrcMode);
    DmxHalSetChannelAFMode(ChanId, DMX_AF_DISCARD);
    DmxHalSetChannelCCDiscon(ChanId, DMX_DISABLE);
    DmxHalSetChannelCCRepeatCtrl(ChanId, DMX_ENABLE);
    DmxHalSetChannelPid(ChanId, ChanInfo->ChanPid);
}

/*
 * save and delivery pes private data macros and functions.
 */
#ifdef HI_DMX_PES_EXT_DATA_SUPPORT
 
#define PTS_DTS_FLAG_MASK (0xc0)
#define ESCR_FLAG_MASK (0x20)
#define ES_RATE_FLAG_MASK (0x10)
#define DSM_TRICK_MODE_FLAG_MASK (0x08)
#define ADD_COPY_INFO_FLAG_MASK (0x04)
#define PES_CRC_FLAG_MASK (0x02)
#define PES_EXTENSION_FLAG_MASK (0x01)
#define PES_EXTENSION2_FLAG_MASK (0x01)
#define PES_PRIV_DATA_FLAG_MASK (0x80)
#define PES_HEADER_FIELD_FLAG_MASK (0x40)
#define PES_PRO_PACK_SEQ_FLAG_MASK (0x20)
#define PES_PSTD_BUFFER_FLAG_MASK (0x10)



#define PTS_DTS_PAYLOAD_01_TYPE_SIZE (5)  /* BYTES */    
#define PTS_DTS_PAYLOAD_11_TYPE_SIZE (10)  /* BYTES */
#define ESCR_PAYLOAD_SIZE (6)  /* BYTES */ 
#define ES_RATE_PAYLOAD_SIZE (3)  /* BYTES */
#define DSM_TRICK_MODE_PAYLOAD_SIZE (1) /* BYTES */ 
#define ADD_COPY_INFO_PAYLOAD_SIZE (1)  /* BYTES */ 
#define PES_CRC_PAYLOAD_SIZE (2)  /* BYTES */
#define PES_PRIVATE_DATA_SIZE (16)  /* BYTES */ 
#define PES_PRO_PACK_SEQ_SIZE (2)   /* BYTES */
#define PES_PSTD_BUFFER_SIZE (2)    /* BYTES */



static inline HI_U32 CalcPtsDtsPayloadSize(HI_U8 flag)
{
    HI_U8 pts_dts_flag = (flag & PTS_DTS_FLAG_MASK) >> 6;
    HI_U32 size;

    switch(pts_dts_flag)
    {
        case 2: /* 1 0 */
            size = PTS_DTS_PAYLOAD_01_TYPE_SIZE;
            break;
        case 3: /* 1 1 */
            size = PTS_DTS_PAYLOAD_11_TYPE_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcESCRPayloadSize(HI_U8 flag)
{
    HI_U8 escr_flag = (flag & ESCR_FLAG_MASK) >> 5; 
    HI_U32 size;

    switch(escr_flag)
    {
        case 1: 
            size = ESCR_PAYLOAD_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}
 
static inline HI_U32 CalcEsRatePayloadSize(HI_U8 flag)
{
    HI_U8 es_rate_flag = (flag & ES_RATE_FLAG_MASK) >> 4; 
    HI_U32 size;

    switch(es_rate_flag)
    {
        case 1: 
            size = ES_RATE_PAYLOAD_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcDSMTrickModePayloadSize(HI_U8 flag)
{
    HI_U8 trick_mode_flag = (flag & DSM_TRICK_MODE_FLAG_MASK) >> 3; 
    HI_U32 size;

    switch(trick_mode_flag)
    {
        case 1: 
            size = DSM_TRICK_MODE_PAYLOAD_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcAddCopyInfoPayloadSize(HI_U8 flag)
{
    HI_U8 copy_info_flag = (flag & ADD_COPY_INFO_FLAG_MASK) >> 2; 
    HI_U32 size;

    switch(copy_info_flag)
    {
        case 1: 
            size = ADD_COPY_INFO_PAYLOAD_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcPesCrcPayloadSize(HI_U8 flag)
{
    HI_U8 pes_crc_flag = (flag & PES_CRC_FLAG_MASK) >> 1; 
    HI_U32 size;

    switch(pes_crc_flag)
    {
        case 1: 
            size = PES_CRC_PAYLOAD_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcPesPrivateDataSize(HI_U8 flag)
{
    HI_U8 pes_pri_flag = (flag & PES_PRIV_DATA_FLAG_MASK) >> 7; 
    HI_U32 size;

    switch(pes_pri_flag)
    {
        case 1: 
            size = PES_PRIVATE_DATA_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcPesHeaderFieldSize(HI_U8 flag, HI_U8 *pu8PesPrivFlagAddr)
{
    HI_U8 pes_head_flag = (flag & PES_HEADER_FIELD_FLAG_MASK) >> 6; 
    HI_U32 size;

    switch(pes_head_flag)
    {
        case 1: 
            /*sizeof(pach_file_length) is 1 */
            size = 1+pu8PesPrivFlagAddr[1+PES_PRIVATE_DATA_SIZE];
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcProPackSeqCountSize(HI_U8 flag)
{
    HI_U8 pes_pro_flag = (flag & PES_PRO_PACK_SEQ_FLAG_MASK) >> 5; 
    HI_U32 size;

    switch(pes_pro_flag)
    {
        case 1: 
            size = PES_PRO_PACK_SEQ_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}

static inline HI_U32 CalcPSTDBufferSize(HI_U8 flag)
{
    HI_U8 pes_pstd_flag = (flag & PES_PSTD_BUFFER_FLAG_MASK) >> 4; 
    HI_U32 size;

    switch(pes_pstd_flag)
    {
        case 1: 
            size = PES_PSTD_BUFFER_SIZE;
            break;
        default:
            size = 0;
            break;
    }

    return size;
}
#endif

static HI_S32 ParsePesHeader(
        DMX_ChanInfo_S *pChanInfo,
        HI_U32          u32ParserAddr,
        HI_U32          PESLength,
        HI_U32         *pPESHeadLen,
        Disp_Control_t *pDispController
    )
{
    HI_U8 *pData;
    HI_U32 PTSFlag;
    HI_U32 PesHeaderLength;

    HI_U32 PTSLow, StreamId;

    pData = (HI_U8*)(u32ParserAddr);
    if ((pData[0] != 0x00) || (pData[1] != 0x00) || (pData[2] != 0x01))
    {
        HI_WARN_DEMUX("pes start byte =0x%x%x%x\n", pData[2], pData[1], pData[0]);
        return HI_FAILURE;
    }

    /* Get StreamId */
    StreamId = pData[3];

    //del pes header according to 13818-1
    if ((StreamId != 0xbc) //1011 1100  1   program_stream_map
        && (StreamId != 0xbe) //1011 1110       padding_stream
        && (StreamId != 0xbf) //1011 1111   3   private_stream_2
        && (StreamId != 0xf0) //1111 0000   3   ECM_stream
        && (StreamId != 0xf1) //1111 0001   3   EMM_stream
        && (StreamId != 0xff) //1111 1111   4   program_stream_directory
        && (StreamId != 0xf2) //1111 0010   5   ITU-T Rec. H.222.0 | ISO/IEC 13818-1 Annex A or ISO/IEC 13818-6_DSMCC_stream
        && (StreamId != 0xf8)) //1111 1000  6   ITU-T Rec. H.222.1 type E
    {
        /*      if (PTS_DTS_flags =='10' ) {
            '0010'  4   bslbf
            PTS [32..30]    3   bslbf
            marker_bit  1   bslbf
            PTS [29..15]    15  bslbf
            marker_bit  1   bslbf
            PTS [14..0] 15  bslbf
            marker_bit  1   bslbf
        }
                if (PTS_DTS_flags =='11' )  {
            '0011'
            PTS [32..30]
            marker_bit
            PTS [29..15]
            marker_bit
            PTS [14..0]
            marker_bit
            '0001'
            DTS [32..30]
            marker_bit
            DTS [29..15]
            marker_bit
            DTS [14..0]
            marker_bit
        }
         */
        PTSFlag = (pData[7] & 0x80) >> 7;
        PesHeaderLength = pData[8];

        *pPESHeadLen = DMX_PES_HEADER_LENGTH + PesHeaderLength;

        if (PTSFlag && (PESLength > *pPESHeadLen))
        {
            PTSLow = (((pData[9] & 0x06) >> 1) << 30)
                     | ((pData[10]) << 22)
                     | (((pData[11] & 0xfe) >> 1) << 15)
                     | ((pData[12]) << 7)
                     | (((pData[13] & 0xfe)) >> 1);
            if (pData[9] & 0x08)
            {
                pChanInfo->LastPts = (PTSLow / 90) + 0x2D82D83; //(1 << 32) / 90 ,使用科学计算器计算结果是0x2D82D83 (0x2D82D83);
            }
            else
            {
                pChanInfo->LastPts = (PTSLow / 90);
            }
        }
        else
        {
            pChanInfo->LastPts = INVALID_PTS;
        }

        /* save pes private data */
#ifdef HI_DMX_PES_EXT_DATA_SUPPORT
        mutex_lock(&(pChanInfo->stDrvPesExtData.pes_lock));
        if (PES_EXTENSION_FLAG_MASK & pData[7] && (PESLength > *pPESHeadLen))
        {
            HI_U32 u32PesExtData1Offset = CalcPtsDtsPayloadSize(pData[7]) + CalcESCRPayloadSize(pData[7])
                                            + CalcEsRatePayloadSize(pData[7]) + CalcDSMTrickModePayloadSize(pData[7])
                                            + CalcAddCopyInfoPayloadSize(pData[7]) + CalcPesCrcPayloadSize(pData[7]);
            HI_U8 *pu8PesExt1Data = &pData[9 + u32PesExtData1Offset];

            if (PES_PRIV_DATA_FLAG_MASK & *pu8PesExt1Data)
            {
                HI_U8  *pu8PesPrivData = pu8PesExt1Data + 1;
                
                memcpy(pChanInfo->stDrvPesExtData.stPesExtData.au8PesExt1Data, pu8PesPrivData, PES_EXT1_DATA_MAXSIZE);
                pChanInfo->stDrvPesExtData.stPesExtData.u16PesExt1DataLen = PES_EXT1_DATA_MAXSIZE;
				if (HI_MPI_DMX_PES_EXT1_DATA_TYPE & pChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType)
				{
					HI_WARN_DEMUX("pes extension data1 overlay\n");
				}
				
                pChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType |= HI_MPI_DMX_PES_EXT1_DATA_TYPE;
            }

            if (PES_EXTENSION2_FLAG_MASK & *pu8PesExt1Data)
            {
                HI_U32 u32PesExtData2Offset = CalcPesPrivateDataSize(*pu8PesExt1Data) 
                                         + CalcPesHeaderFieldSize(*pu8PesExt1Data, pu8PesExt1Data)
                                         + CalcProPackSeqCountSize(*pu8PesExt1Data)
                                         + CalcPSTDBufferSize(*pu8PesExt1Data);
                HI_U8 *pu8PesExt2Data = &pData[9+u32PesExtData1Offset+1+u32PesExtData2Offset];
                HI_U8 u8PesExt2DataLen = *pu8PesExt2Data >> 1;
                pChanInfo->stDrvPesExtData.stPesExtData.u16PesExt1DataLen = u8PesExt2DataLen;
                memcpy(pChanInfo->stDrvPesExtData.stPesExtData.au8PesExt2Data, pu8PesExt2Data+1, u8PesExt2DataLen);
                if (HI_MPI_DMX_PES_EXT2_DATA_TYPE & pChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType)
				{
					HI_WARN_DEMUX("pes extension data2 overlay\n");
				}
				pChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType |= HI_MPI_DMX_PES_EXT2_DATA_TYPE;
                
            }
        }
        mutex_unlock(&(pChanInfo->stDrvPesExtData.pes_lock));
#endif
        
        //add by yangchun 201302234
        if((0xee == StreamId) && ( PESLength > 24)) 
        {
            if( (0x70 == pData[13]) && (0x76 == pData[14]) && (0x72 == pData[15]) && (0x63 == pData[16]))
            {
                if( (0x75 == pData[17]) && (0x72 == pData[18]) && (0x74 == pData[19]) && (0x6d == pData[20]))
                {
                    pDispController->u32DispTime = *(HI_U32 *)&pData[21];
                    pDispController->u32DispEnableFlag = *(HI_U32 *)&pData[25];
                    pDispController->u32DispFrameDistance = *(HI_U32 *)&pData[29];
                    pDispController->u32DistanceBeforeFirstFrame = *(HI_U32 *)&pData[33];
                    pDispController->u32GopNum = *(HI_U32 *)&pData[37];
                    *pPESHeadLen = 184;
                    return -2;
                }
            }
        }

        return HI_SUCCESS;
    }
    else
    {
        HI_ERR_DEMUX("streamID:%x did not del the pes header!\n", StreamId);
        return HI_FAILURE;
    }
}

static HI_S32 DMXCheckBuffer(  HI_U32 u32BufStartAddr,
                               HI_U32 u32BufLen,
                               HI_U32 u32Offset)
{
    HI_U32 u32SecLen, u32PhyAddr, u32BufLenTmp;

    u32BufLenTmp = u32BufLen - u32Offset;
    while (u32BufLenTmp > 3)
    {
        u32SecLen  = 3;
        u32PhyAddr = u32BufStartAddr + u32Offset;
        if (u32BufLen < u32Offset)
        {
            HI_ERR_DEMUX("invalide offset,u32BufLen:%d,u32Offset:%d!\n",u32BufLen,u32Offset);
            return -1;
        }

        u32SecLen += (((*(HI_U8 *)(u32PhyAddr + 1)) & 0xF) << 8);
        u32SecLen += *(HI_U8 *)(u32PhyAddr + 2);
        if ((u32SecLen > u32BufLenTmp) || (u32SecLen > DMX_MAX_SEC_LEN))
        {
            HI_ERR_DEMUX("invalid u32SecLen, u32SecLen:%d,u32BufLenTmp:%d!\n",u32SecLen,u32BufLenTmp);
            return -1;
        }

        u32Offset    += u32SecLen;
        u32BufLenTmp -= u32SecLen;
    }

    return 0;
}

HI_U32 GetSectionLength(HI_U32 u32BufStartAddr, HI_U32 u32BufLen, HI_U32 u32Offset)
{
    HI_U32 u32PacketLen = 0;
    HI_U32 u32BufAddr, u32EndAddr;

    if (0 == u32BufStartAddr)
    {
        HI_ERR_DEMUX("Buffer start addr is 0!\n");
        return 0;
    }

    if (0 == u32BufLen)
    {
        HI_ERR_DEMUX("Buffer len is 0!\n");
        return 0;
    }

    if (u32Offset >= u32BufLen)
    {
        HI_ERR_DEMUX("u32Offset is larger than buflen,%x!\n", u32Offset);
        return 0;
    }

    if (DMXCheckBuffer(u32BufStartAddr, u32BufLen, u32Offset) != 0)
    {
        return 0;
    }

    if (u32BufLen >= 3)
    {
        u32PacketLen = 3;
        u32BufAddr = u32BufStartAddr + u32Offset;
        u32EndAddr = u32BufStartAddr + u32BufLen;
        if (u32BufAddr >= u32EndAddr)
        {
            HI_ERR_DEMUX("u32Offset is larger than buflen!\n");
            return 0;
        }

        u32PacketLen += (((*(HI_U8 *)(u32BufAddr + 1)) & 0xF) << 8);
        if (u32BufAddr >= u32EndAddr)
        {
            HI_ERR_DEMUX("u32Offset is larger than buflen!\n");
            return 0;
        }

        u32PacketLen += *(HI_U8 *)(u32BufAddr + 2);
        if ((u32BufLen - u32Offset) < u32PacketLen)
        {
            u32PacketLen = 0;
        }
    }

    return u32PacketLen;
}

static HI_S32 RamPortClean(HI_U32 PortId)
{
    HI_U32              i;
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];

    for (i = 0; i < DMX_CNT; i++)
    {
        if (DMX_PORT_MODE_TUNER == g_pDmxDevOsi->SubDevInfo[i].PortMode)
        {
            continue;
        }

        if (PortId == g_pDmxDevOsi->SubDevInfo[i].PortId)
        {
            /* Select the dmx port to invalid, to flush remain data*/
            DmxHalSetDemuxPortId(i, DMX_PORT_MODE_BUTT, DMX_INVALID_PORT_ID);
        }
    }

    DmxHalSetFlushIPPort(PortId);
    DmxHalClrIPBPStatus(PortId);

    udelay(10);

    for (i = 0; i < DMX_CNT; i++)
    {
        if (DMX_PORT_MODE_TUNER == g_pDmxDevOsi->SubDevInfo[i].PortMode)
        {
            continue;
        }

        if (PortId == g_pDmxDevOsi->SubDevInfo[i].PortId)
        {
            /* Select the dmx port to set back*/
            DmxHalSetDemuxPortId(i, DMX_PORT_MODE_RAM, PortId);
        }
    }

    PortInfo->Read  = 0;
    PortInfo->Write = 0;

    PortInfo->DescWrite     = 0;
    PortInfo->DescCurrAddr  = (HI_U32*)PortInfo->DescKerAddr;

    PortInfo->GetCount      = 0;
    PortInfo->GetValidCount = 0;
    PortInfo->PutCount      = 0;

    return HI_SUCCESS;
}

static HI_VOID GetIPBufLen(const HI_U32 PortId, const HI_U32 ReqLen)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];
    HI_U32              Head;
    HI_U32              Tail;

    PortInfo->ReqAddr   = 0;
    PortInfo->ReqLen    = 0;

    if (PortInfo->Read < PortInfo->Write)
    {
        Head = PortInfo->Read;
        Tail = PortInfo->BufSize - PortInfo->Write;

        Head = (Head <= DMX_TS_BUFFER_GAP) ? 0 : (Head - DMX_TS_BUFFER_GAP);
        Tail = (Tail <= DMX_TS_BUFFER_GAP) ? 0 : (Tail - DMX_TS_BUFFER_GAP);

        if (Tail >= ReqLen)
        {
            PortInfo->ReqAddr = PortInfo->PhyAddr + PortInfo->Write;
        }
        else if (Head >= ReqLen)
        {
            PortInfo->ReqAddr = PortInfo->PhyAddr;
        }
    }
    else if (PortInfo->Read > PortInfo->Write)
    {
        Head = PortInfo->Read - PortInfo->Write;
        if (Head > DMX_TS_BUFFER_GAP)
        {
            Head -= DMX_TS_BUFFER_GAP;

            if (Head >= ReqLen)
            {
                PortInfo->ReqAddr = PortInfo->PhyAddr + PortInfo->Write;
            }
        }
    }
    else
    {
        if (0 == PortInfo->Read)
        {
            PortInfo->ReqAddr = PortInfo->PhyAddr;
        }
    }

    if (PortInfo->ReqAddr)
    {
        PortInfo->ReqLen = ReqLen;
    }
}

/**
 \brief

 \attention

 \param[in] u32PortId
 \param[in] u32DataSize

 \retval none
 \return none

 \see
 \li ::
 */
static HI_BOOL CheckIPBuf(const HI_U32 PortId, const HI_U32 ReqLen)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];
    HI_U32              DescRead;
    HI_U32              AddDesc;
    HI_U32              FreeDesc;
    HI_SIZE_T           LockFlag;

    AddDesc  = ReqLen / DMX_MAX_IP_BLOCK_SIZE + 2;

    DescRead = DmxHalIPPortDescGetRead(PortId);

    FreeDesc = PortInfo->DescDepth - GetQueueLenth(DescRead, PortInfo->DescWrite, PortInfo->DescDepth);
    if (AddDesc >= FreeDesc)
    {
        HI_WARN_DEMUX("no free desc. add=0x%x, free=0x%x, r=0x%x, w=0x%x, Size=0x%x\n",
            AddDesc, FreeDesc, DescRead, PortInfo->DescWrite, ReqLen);

        return HI_TRUE;
    }

    spin_lock_irqsave(&PortInfo->LockRamPort, LockFlag);
    GetIPBufLen(PortId, ReqLen);
    spin_unlock_irqrestore(&PortInfo->LockRamPort, LockFlag);

    if (0 == PortInfo->ReqAddr)
    {
        HI_WARN_DEMUX("buffer not enougth, r=0x%x, w=0x%x, size=0x%x\n",
            PortInfo->Read, PortInfo->Write, PortInfo->BufSize);

        return HI_TRUE;
    }

    return HI_FALSE;
}

HI_S32 DMX_OsiResetChannel(HI_U32 ChanId, DMX_FLUSH_TYPE_E eFlushType)
{
    DMX_DEV_OSI_S  *pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo;
    DMX_OQ_Info_S  *OqInfo;
    HI_S32 s32OQEnableStatus;

    IsDmxDevInit();
    pDmxDevOsi = g_pDmxDevOsi;

    ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    OqInfo      = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

    if (DMX_FLUSH_TYPE_REC & eFlushType)
    {
        DmxFlushChannel(ChanId, DMX_FLUSH_TYPE_REC);

        /*if only close rec channel and flush rec*/
        if (DMX_FLUSH_TYPE_REC == eFlushType)
        {
            return HI_SUCCESS;
        }
    }

    s32OQEnableStatus = DmxHalGetOQEnableStatus(ChanInfo->ChanOqId);
    DmxHalDisableOQRecive(ChanInfo->ChanOqId);
    DmxFlushChannel(ChanId, eFlushType);
    DmxFlushOq(ChanInfo->ChanOqId, DMX_OQ_CLEAR_TYPE_PLAY);

    if(s32OQEnableStatus)
    {
        DmxHalEnableOQRecive(ChanInfo->ChanOqId);
    }

    ResetOQ(ChanInfo->DmxId, OqInfo);

    DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[ChanInfo->DmxId], OqInfo->u32FQId);

    //DMXCheckOQEnableStatus(pChanInfo->stChBuf.u32OQId, pChanInfo->stChBuf.u32OQDepth);
    return HI_SUCCESS;
}

/**
 \brief
 \attention

 \param[in] u32Read
 \param[in] u32Write
 \param[in] IsAddrValid

 \retval none
 \return HI_SUCCESS
 \return HI_FAILURE


 \see
 \li ::
 */
static HI_S32 IsAddrValid( HI_U32 u32Read, HI_U32 u32Write, HI_U32 u32Addr)
{
    if (u32Read == u32Write)
    {
        HI_WARN_DEMUX("Read == Write,No Data, R=0x%x, W=0x%x, J=0x%x!\n", u32Read, u32Write, u32Addr);
        return HI_FAILURE;
    }

    if (u32Read < u32Write)
    {
        if ((u32Read < u32Addr) && (u32Write >= u32Addr))
        {
            return HI_SUCCESS;
        }
        else
        {
            HI_ERR_DEMUX("Addr is out of range, R=0x%x, W=0x%x, J=0x%x!\n", u32Read, u32Write, u32Addr);
            return HI_FAILURE;
        }
    }
    else
    {
        if ((u32Read < u32Addr) || (u32Write >= u32Addr))
        {
            return HI_SUCCESS;
        }
        else
        {
            HI_ERR_DEMUX("Addr is out of range, R=0x%x, W=0x%x, J=0x%x!\n", u32Read, u32Write, u32Addr);
            return HI_FAILURE;
        }
    }
}

static HI_U32 DMXOsiGetCurEopaddr(DMX_ChanInfo_S *ChanInfo);
static HI_S32 DMXOsiFindPes(DMX_ChanInfo_S * pChanInfo,
                            DMX_UserMsg_S* psMsgList,
                            HI_U32         u32AcqNum,
                            HI_U32 *       pu32AcqedNum);
static HI_U32 GetPesChnFlag(DMX_ChanInfo_S * pChanInfo)
{
#if 0
    HI_U32 u32CurrentBlk,u32BufPhyAddr,u32PvrCtrl;
    HI_U8 *pu8BufVirAddr;
    HI_U32 u32WriteBlk,u32ReadBlk;
    OQ_DescInfo_S* oq_desc;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    HI_U32 u32OQlen,u32ValidLen = 0;
    HI_U32 u32PesLen = DMX_PES_MAX_SIZE;
    HI_S32 s32PesStartPos = -1;
    HI_U32 i;
    HI_U32 u32DropTimes = 0;

    DMXOsiOQGetReadWrite(pChanInfo->stChBuf.u32OQId, &u32WriteBlk, &u32ReadBlk);

    u32OQlen = GetQueueLenth(u32ReadBlk,u32WriteBlk,pChanInfo->stChBuf.u32OQDepth);
    for ( i = 0 ; i <  u32OQlen; i++ )
    {
        u32CurrentBlk = pChanInfo->stChBuf.u32OQVirAddr + u32ReadBlk * DMX_OQ_DESC_SIZE;
        oq_desc = (OQ_DescInfo_S*)u32CurrentBlk;
        u32BufPhyAddr = oq_desc->start_addr;
        u32PvrCtrl = oq_desc->pvrctrl_datalen;
        u32ValidLen++;
        if (u32PvrCtrl & DMX_MASK_BIT_18)//sop
        {
            if (s32PesStartPos > 0)
            {
                return 1;//reach 2nd sop
            }
            else if (u32ValidLen > 1)
            {
                return 1;//last pes end
            }
            s32PesStartPos = i;
            u32ValidLen = 1;
            pu8BufVirAddr = (HI_U8 *)(u32BufPhyAddr
            - pDmxDevOsi->DmxFqInfo[pChanInfo->stChBuf.u32FQId].u32BufPhyAddr
            + pDmxDevOsi->DmxFqInfo[pChanInfo->stChBuf.u32FQId].u32BufVirAddr);
            u32PesLen = ((HI_U32)pu8BufVirAddr[4] << 8) | pu8BufVirAddr[5];
            if (u32PesLen)
            {
                u32PesLen += 6;
            }
            else
            {
                u32PesLen = DMX_PES_MAX_SIZE;
            }
        }
        else if (u32PvrCtrl & DMX_MASK_BIT_19)//drop
        {
            if (s32PesStartPos > 0)//reach 2nd sop
            {
                s32PesStartPos= -1;
            }
            u32PesLen = DMX_PES_MAX_SIZE;
            u32ValidLen = 0;
            u32DropTimes++;
            if (u32DropTimes > 10)
            {
                return 1;
            }
        }
        if (u32ValidLen * DMX_FQ_COM_BLKSIZE >= u32PesLen)
        {
            return 1;
        }
        DMXINC(u32ReadBlk, pChanInfo->stChBuf.u32OQDepth);
    }

    return 0;
#else
    DMX_ChanInfo_S stTmpChanInfo;
    HI_U32 u32AcqedNum;
    HI_U32 u32CurDropCnt;
    DMX_UserMsg_S stMsgList[16];
    HI_S32 s32Ret;
    u32CurDropCnt = s_u32DMXDropCnt[pChanInfo->ChanId];
    memcpy(&stTmpChanInfo,pChanInfo,sizeof(DMX_ChanInfo_S));
    pChanInfo->u32ChnResetLock = 1;
    s32Ret = DMXOsiFindPes(&stTmpChanInfo,stMsgList,16, &u32AcqedNum);
    if (u32CurDropCnt != s_u32DMXDropCnt[pChanInfo->ChanId])//drop occurs
    {
        pChanInfo->u32PesBlkCnt   = 0;
        pChanInfo->u32PesLength   = 0;
        pChanInfo->u32ProcsOffset = 0;
    }
    pChanInfo->u32ChnResetLock = 0;
    if (HI_SUCCESS == s32Ret)
    {
        return 1;
    }
    return 0;
#endif
}

HI_U32 DMX_OsiGetChnDataFlag(HI_U32 ChanId)
{
    HI_U32 u32OqId, u32OqRead, u32OqWrite, u32CurProcsBlk;
    HI_U32 u32LastEopAddr;
    DMX_DEV_OSI_S  *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo;
    DMX_OQ_Info_S  *OqInfo;
    HI_U32 u32CurrentBlk, u32DataLen;
    OQ_DescInfo_S* oq_dsc;

    ChanInfo = &pDmxDevOsi->DmxChanInfo[ChanId];
    if (ChanInfo->DmxId >= DMX_CNT)
    {
        return 0;
    }

    if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
    {
        return 0;   // do not receive vid and aud channel
    }

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY != (ChanInfo->ChanOutMode & HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY))
    {
        return 0;   // do not receive rec channel
    }

    OqInfo = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

    u32OqId = ChanInfo->ChanOqId;
    u32CurProcsBlk = OqInfo->u32ProcsBlk;
    DMXOsiOQGetReadWrite(u32OqId, &u32OqWrite, &u32OqRead);

    u32LastEopAddr = DMXOsiGetCurEopaddr(ChanInfo);

    if (u32OqRead != u32OqWrite)
    {
        if (HI_UNF_DMX_CHAN_TYPE_PES == ChanInfo->ChanType) //pes channel just care about read and write ptr
        {
            if (GetPesChnFlag(ChanInfo))
            {
                return 1;
            }
            return 0;
        }

        if (u32OqWrite != u32CurProcsBlk)
        {
            DMXINC(u32CurProcsBlk, OqInfo->u32OQDepth);
            if (u32OqWrite == u32CurProcsBlk)
            {
                u32CurrentBlk = OqInfo->u32OQVirAddr
                                + OqInfo->u32ProcsBlk * DMX_OQ_DESC_SIZE;
                oq_dsc = (OQ_DescInfo_S*)u32CurrentBlk;
                if (!oq_dsc)
                {
                    HI_ERR_DEMUX("DMXOsiGetSecNum oqdsc error\n");
                    return 0;
                }

                u32DataLen = oq_dsc->pvrctrl_datalen & 0xffff;
                if (OqInfo->u32ProcsOffset < u32DataLen)
                {
                    return 1;
                }
                else if (u32LastEopAddr)
                {
                    return 1;
                }
            }
            else
            {
                return 1;
            }
        }
    }

    if (HI_UNF_DMX_CHAN_TYPE_PES != ChanInfo->ChanType)
    {
        if ((u32LastEopAddr != OqInfo->u32ProcsOffset) && (u32LastEopAddr != 0))
        {
            return 1;
        }
    }

    return 0;
}

static HI_VOID DMXOsiOQGetReadWrite(HI_U32 OQId, HI_U32 *BlockWrite, HI_U32 *BlockRead)
{
    HI_U32 Value;

    DmxHalGetOQWORDx(OQId, DMX_OQ_RDWR_OFFSET, &Value);

    if (BlockWrite)
    {
        *BlockWrite = Value & DMX_OQ_DEPTH;
    }

    if (BlockRead)
    {
        *BlockRead = (Value >> 16) & DMX_OQ_DEPTH;
    }
}

static HI_S32 DMXOsiOQGetBbsAddrSize(HI_U32 OqId, HI_U32 OqWrite, HI_U32 *BbsAddr, HI_U32 *Size, HI_U32 *PvrCtrl)
{
    HI_S32  ret= HI_FAILURE;
    HI_U32  Value;
    HI_U32  CurrWrite;
    HI_U32  PhyAddr;
    HI_U32  len;

    *BbsAddr    = 0;
    *Size       = 0;
    *PvrCtrl    = 0;

    DmxHalGetOQWORDx(OqId, DMX_OQ_CTRL_OFFSET, &Value);

    Value &= 0xff;

    DmxHalGetOQWORDx(OqId, DMX_OQ_SADDR_OFFSET, &PhyAddr);

    DmxHalGetOQWORDx(OqId, DMX_OQ_EOPWR_OFFSET, &len);

    len &= 0xffff;

    DmxHalGetOQWORDx(OqId, DMX_OQ_RDWR_OFFSET, &CurrWrite);

    CurrWrite &= DMX_OQ_DEPTH;

    if ((OqWrite == CurrWrite) && (Value & DMX_MASK_BIT_7) && PhyAddr && len)
    {
        *BbsAddr    = PhyAddr;
        *Size       = len;
        *PvrCtrl    = Value;

        ret = HI_SUCCESS;
    }

    return ret;
}

static HI_VOID DMXOsiRelaseOQ(DMX_OQ_Info_S *pstChPlayBuf, HI_U32 u32ReadBlk, HI_U32 u32Num)
{
    HI_U32  u32CurrentBlk;
    HI_U32  FqId;
    HI_U32  fqwrite, oqread;
    OQ_DescInfo_S* oq_desc;
    FQ_DescInfo_S* fq_desc;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo;
    HI_SIZE_T u32LockFlag;

    FqId = pstChPlayBuf->u32FQId;

    FqInfo = &pDmxDevOsi->DmxFqInfo[FqId];

    spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);

    fqwrite = DmxHalGetFQWritePtr(FqId);

    oqread = u32ReadBlk;
    while (u32Num)
    {
        u32CurrentBlk = pstChPlayBuf->u32OQVirAddr + oqread * DMX_OQ_DESC_SIZE;
        oq_desc = (OQ_DescInfo_S*)u32CurrentBlk;
        u32CurrentBlk = pDmxDevOsi->DmxFqInfo[FqId].u32FQVirAddr + fqwrite * DMX_FQ_DESC_SIZE;
        fq_desc = (FQ_DescInfo_S*)u32CurrentBlk;
        fq_desc->start_addr = oq_desc->start_addr;
        fq_desc->buflen = oq_desc->cactrl_buflen & 0xffff;
        DMXINC(oqread, pstChPlayBuf->u32OQDepth);
        DMXINC(fqwrite, pDmxDevOsi->DmxFqInfo[FqId].u32FQDepth);
        u32Num--;
    }

    DmxHalSetOQReadPtr(pstChPlayBuf->u32OQId, oqread);
    pstChPlayBuf->u32ReleaseBlk = oqread;
    DmxHalSetFQWritePtr(FqId, fqwrite);

    spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
}

static HI_S32 DMXOsiFindPes(DMX_ChanInfo_S * pChanInfo,
                            DMX_UserMsg_S* psMsgList,
                            HI_U32         u32AcqNum,
                            HI_U32 *       pu32AcqedNum)
{
    HI_U32 u32CurrentBlk;
    HI_U32 u32BufPhyAddr, u32PvrCtrl, u32DataLen;
    HI_U8 *pu8BufVirAddr;
    HI_U32 u32WriteBlk, u32ReadBlk, u32CurReadBlk;
    HI_U32 u32CurBlkNum, u32ProcsBlk;
    HI_U32 u32PesHead = 0;
    HI_U32 u32Offset, u32PesLen;
    HI_U32 u32DropTimes = 0;
    OQ_DescInfo_S* oq_desc;
    DMX_DEV_OSI_S  *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_OQ_Info_S  *OqInfo      = &pDmxDevOsi->DmxOqInfo[pChanInfo->ChanOqId];

    //always get new write
    DMXOsiOQGetReadWrite(pChanInfo->ChanOqId, &u32WriteBlk, &u32ReadBlk);

    u32ProcsBlk = pChanInfo->u32PesBlkCnt;
    u32Offset = pChanInfo->u32ProcsOffset;
    u32PesLen = pChanInfo->u32PesLength;
    u32CurReadBlk = u32ReadBlk;
    u32CurBlkNum  = 0;
    *pu32AcqedNum = 0;
    while (u32ReadBlk != u32WriteBlk)
    {
        u32CurrentBlk = OqInfo->u32OQVirAddr + u32ReadBlk * DMX_OQ_DESC_SIZE;
        oq_desc = (OQ_DescInfo_S*)u32CurrentBlk;
        u32BufPhyAddr = oq_desc->start_addr;
        u32PvrCtrl = oq_desc->pvrctrl_datalen;
        u32DataLen = oq_desc->pvrctrl_datalen & 0xffff;
        if (u32PvrCtrl & DMX_MASK_BIT_18)
        {
            if (u32ProcsBlk == 0)
            {
                pu8BufVirAddr = (HI_U8 *)(u32BufPhyAddr
                                          - pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId].u32BufPhyAddr
                                          + pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId].u32BufVirAddr);

                //according to protocol
                u32PesLen = ((HI_U32)pu8BufVirAddr[4] << 8) | pu8BufVirAddr[5];
                if (u32PesLen)
                {
                    u32PesLen += 6;
                }

                u32PesHead = 1;
                pChanInfo->u32PesLength = u32PesLen;
            }
            else
            {
                //found next sop,so get full pes
                if (u32CurBlkNum)
                {
                    if (pChanInfo->u32PesBlkCnt)
                    {
                        psMsgList[u32CurBlkNum - 1].enDataType = HI_UNF_DMX_DATA_TYPE_TAIL;
                    }
                    else
                    {
                        psMsgList[u32CurBlkNum - 1].enDataType = HI_UNF_DMX_DATA_TYPE_WHOLE;
                    }

                    pChanInfo->u32PesLength   = 0;
                    pChanInfo->u32ProcsOffset = 0;
                    pChanInfo->u32PesBlkCnt = 0;
                    *pu32AcqedNum = u32CurBlkNum;
                    return HI_SUCCESS;
                }
                /*
                situation such as: 64K PES with len fild = 0,
                --------------------1PES-----------------------|------------------2PES-------------------------
                |sop(8k)| 8k  |  8k  | 8k |8k  |  8k  | 8k |8k | sop(8k)| 8k  |  8k  | 8k |8k  |  8k  | 8k |8k |
                when the second time meet BB with SOP ,now : u32ProcsBlk == 8, u32CurBlkNum == 0, will enter this else
                pay attentaion: in this situation ,the last time DMXOsiFindPes return ,may be the psMsgList[7].enDataType is BODY,
                acturally it should be Tail. but we have no idea how to change it before user use it.
                so ,just let it be ,you should know this may be error.
                */
                else
                {
                    u32ProcsBlk = 0;
                    pChanInfo->u32PesBlkCnt = 0;
                    pChanInfo->u32PesLength   = 0;
                    pChanInfo->u32ProcsOffset = 0;
                    continue;
                }
            }
        }

        //if drop,need test
        if (u32PvrCtrl & DMX_MASK_BIT_19)
        {
            DMXOsiRelaseOQ(OqInfo, u32CurReadBlk, u32CurBlkNum + 1);
            DMXOsiOQGetReadWrite(pChanInfo->ChanOqId, &u32WriteBlk, &u32ReadBlk);
            u32CurReadBlk = u32ReadBlk;
            u32CurBlkNum = 0;
            u32ProcsBlk = 0;
            pChanInfo->u32PesBlkCnt   = 0;
            pChanInfo->u32PesLength   = 0;
            pChanInfo->u32ProcsOffset = 0;
            u32Offset = 0;
            u32DropTimes++;
            s_u32DMXDropCnt[pChanInfo->ChanId]++;
            if (u32DropTimes > 10)
            {
                return HI_FAILURE;
            }
            else
            {
                continue;
            }
        }

        u32Offset += u32DataLen;
        psMsgList[u32CurBlkNum].u32BufStartAddr = u32BufPhyAddr;
        psMsgList[u32CurBlkNum].u32MsgLen = u32DataLen;
        if (u32PesHead)
        {
            psMsgList[u32CurBlkNum].enDataType = HI_UNF_DMX_DATA_TYPE_HEAD;
            u32PesHead = 0;
        }
        else
        {
            psMsgList[u32CurBlkNum].enDataType = HI_UNF_DMX_DATA_TYPE_BODY;
        }
        DMXINC(u32ReadBlk, OqInfo->u32OQDepth);
        OqInfo->u32ProcsBlk = u32ReadBlk;

        if ((u32Offset == u32PesLen) && u32PesLen)
        {
            if (u32ProcsBlk == 0)
            {
                psMsgList[u32CurBlkNum].enDataType = HI_UNF_DMX_DATA_TYPE_WHOLE;
            }
            else
            {
                psMsgList[u32CurBlkNum].enDataType = HI_UNF_DMX_DATA_TYPE_TAIL;
            }

            pChanInfo->u32PesLength   = 0;
            pChanInfo->u32ProcsOffset = 0;
            pChanInfo->u32PesBlkCnt = 0;
            *pu32AcqedNum = u32CurBlkNum + 1;
            return HI_SUCCESS;
        }

        u32ProcsBlk++;
        u32CurBlkNum++;

        /* if reach 64k and still not find next sop */
        if ((u32CurBlkNum == DMX_PES_MAX_SIZE) || (u32CurBlkNum == u32AcqNum))
        {
            break;
        }
    }

    if ((DMX_PES_MAX_SIZE == u32CurBlkNum) || (u32CurBlkNum == u32AcqNum))
    {
        *pu32AcqedNum = u32CurBlkNum;
        pChanInfo->u32PesBlkCnt   += u32CurBlkNum;
        pChanInfo->u32ProcsOffset += u32Offset;
        return HI_SUCCESS;
    }

    return HI_FAILURE;
}

static HI_S32 DMXOsiAddErrMSG(HI_U32 u32BufStartAddr)
{
    HI_U32 i = 0;

    for (i = 0; i < DMX_MAX_ERRLIST_NUM; i++)
    {
        if ((psErrMsg[i].u32UsedFlag == 1) && (psErrMsg[i].psMsg.u32BufStartAddr == u32BufStartAddr))
        {
            return 0;
        }
    }

    for (i = 0; i < DMX_MAX_ERRLIST_NUM; i++)
    {
        if (psErrMsg[i].u32UsedFlag == 0)
        {
            psErrMsg[i].psMsg.u32BufStartAddr = u32BufStartAddr;
            psErrMsg[i].u32UsedFlag = 1;
            return 0;
        }
    }

    return -1;
}

static HI_S32 DMXOsiRelErrMSG(HI_U32 u32BufStartAddr)
{
    HI_U32 i = 0;

    for (i = 0; i < DMX_MAX_ERRLIST_NUM; i++)
    {
        if ((psErrMsg[i].u32UsedFlag == 1) && (psErrMsg[i].psMsg.u32BufStartAddr == u32BufStartAddr))
        {
            psErrMsg[i].u32UsedFlag = 0;
            return 0;
        }
    }

    return -1;
}

static HI_S32 DMXOsiCheckErrMSG(HI_U32 u32BufStartAddr)
{
    HI_U32 i = 0;

    for (i = 0; i < DMX_MAX_ERRLIST_NUM; i++)
    {
        if ((psErrMsg[i].u32UsedFlag == 1) && (psErrMsg[i].psMsg.u32BufStartAddr == u32BufStartAddr))
        {
            return 1;
        }
    }

    return 0;
}

static HI_S32 DMXOsiParseSection(HI_U32 *pu32Parsed, HI_U32 u32AcqNum, HI_U32 *pu32Offset,
                                 HI_U32 u32BufPhyAddr, HI_U32 u32BufLen, DMX_ChanInfo_S *pChanInfo,
                                 DMX_UserMsg_S* psMsgList,DMX_OQ_Info_S  *OqInfo)
{
    HI_U32 i;
    HI_U32 u32Seclen;
    HI_U32 u32BufVirAddr;
    DMX_DEV_OSI_S  *pDmxDevOsi  = g_pDmxDevOsi;

    
    u32BufVirAddr = u32BufPhyAddr
                    - pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId].u32BufPhyAddr
                    + pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId].u32BufVirAddr;

    if (*pu32Offset >= u32BufLen)
    {
        return HI_FAILURE;
    }

    for (i = *pu32Parsed; i < u32AcqNum;)
    {
        if (HI_UNF_DMX_CHAN_TYPE_POST == pChanInfo->ChanType)
        {
            u32Seclen = DMX_TS_PACKET_LEN;
            if (*(HI_U8 *)(u32BufVirAddr + *pu32Offset) != 0x47)
            {
                DMXOsiAddErrMSG(u32BufPhyAddr);
                s_u32DMXDropCnt[pChanInfo->ChanId]++;
                *pu32Offset += u32Seclen;
                if (*pu32Offset >= u32BufLen)
                {
                    break;
                }
                continue;
            }
        }
#ifdef DMX_USE_ECM
        else if (pChanInfo->u32SwFlag)
        {
            u32Seclen = DMX_TS_PACKET_LEN;
        }
#endif
        else
        {
            u32Seclen = GetSectionLength(u32BufVirAddr, u32BufLen, *pu32Offset);
        }

        if ((0 == u32Seclen) || (u32Seclen > DMX_MAX_SEC_LEN))
        {
#ifdef DMX_USE_ECM
            HI_S32 s32Ret;
#endif

            DMXOsiAddErrMSG(u32BufPhyAddr);
            s_u32DMXDropCnt[pChanInfo->ChanId]++;

            HI_ERR_DEMUX("section error. ChanId=%u, Pid=0x%x, seclen:%x, phyaddr:%x, BufLen:%x, offset:%x\n",
                pChanInfo->ChanId, pChanInfo->ChanPid, u32Seclen, u32BufPhyAddr, u32BufLen, *pu32Offset);

            *pu32Offset = u32BufLen; //jump the error section

#ifdef DMX_USE_ECM
            s32Ret = DMX_OsiCloseChannel(pChanInfo->ChanId);
            if (HI_SUCCESS != s32Ret)
            {
                HI_ERR_DEMUX("close channel error:%x!\n",s32Ret);
            }
            s32Ret = HI_DMX_SwNewChannel(pChanInfo->ChanId);
            if (HI_SUCCESS != s32Ret)
            {
                HI_ERR_DEMUX("create sw channel error:%x!\n",s32Ret);
            }
#endif

            break;
        }
        else if (u32Seclen < 7)
        {
            HI_U32 section_syntax_indicator = (*(HI_U8 *)(u32BufVirAddr + *pu32Offset + 1)) & 0x80;

            if (   (HI_UNF_DMX_CHAN_CRC_MODE_FORCE_AND_DISCARD == pChanInfo->ChanCrcMode)
                || ((HI_UNF_DMX_CHAN_CRC_MODE_BY_SYNTAX_AND_DISCARD == pChanInfo->ChanCrcMode) &&  section_syntax_indicator) )
            {
                if (!*pu32Offset)   //add err addr, only when at the begin of the buffer
                {
                    DMXOsiAddErrMSG(u32BufPhyAddr);
                }

                s_u32DMXDropCnt[pChanInfo->ChanId]++;
                *pu32Offset += u32Seclen;
                if (*pu32Offset >= u32BufLen)
                {
                    break;
                }
                continue;
            }
        }

#ifndef DMX_FILTER_DEPTH_SUPPORT
        if (u32Seclen < DMX_FILTER_MAX_DEPTH + SECTION_LENGTH_FIELD_SIZE)
        {
            HI_U32              FilterId;
            DMX_FilterInfo_S   *FilterInfo  = g_pDmxDevOsi->DmxFilterInfo;
            HI_U32              ChanId      = pChanInfo->ChanId;
            HI_U8              *sect        = (HI_U8*)(u32BufVirAddr + *pu32Offset);
            HI_BOOL             pass        = HI_FALSE;

            for (FilterId = 0; FilterId < DMX_FILTER_CNT; FilterId++)
            {
                HI_U32  len     = 0;
                HI_U32  Depth   = FilterInfo[FilterId].Depth;
                HI_U8  *match   = FilterInfo[FilterId].Match;
                HI_U8  *mask    = FilterInfo[FilterId].Mask;
                HI_U8  *negate  = FilterInfo[FilterId].Negate;
                HI_U32  j;

                if (ChanId != FilterInfo[FilterId].ChanId)
                {
                    continue;
                }

                if (0 == Depth)
                {
                    pass = HI_TRUE;

                    break;
                }

                if (u32Seclen < Depth + SECTION_LENGTH_FIELD_SIZE)
                {
                    continue;
                }

                for (j = 0; j < Depth; j++)
                {
                    if (len == u32Seclen)
                    {
                        break;
                    }

                    if (negate[j])
                    {
                        if ((match[j] & ~mask[j]) == (sect[len] & ~mask[j]))
                        {
                            break;
                        }
                    }
                    else
                    {
                        if ((match[j] & ~mask[j]) != (sect[len] & ~mask[j]))
                        {
                            break;
                        }
                    }

                    if (0 == j)
                    {
                        len = 3;
                    }
                    else
                    {
                        ++len;
                    }
                }

                if (j == Depth)
                {
                    pass = HI_TRUE;

                    break;
                }
            }

            if (!pass)
            {
                DMXOsiAddErrMSG(u32BufPhyAddr);
                s_u32DMXDropCnt[pChanInfo->ChanId]++;
                *pu32Offset += u32Seclen;
                if (*pu32Offset >= u32BufLen)
                {
                    break;
                }
                continue;
            }
        }
#endif

        psMsgList[i].u32BufStartAddr = u32BufPhyAddr + *pu32Offset;
        psMsgList[i].u32MsgLen = u32Seclen;
        *pu32Offset += u32Seclen;
        DMXOsiRelErrMSG(u32BufPhyAddr);
        if (*pu32Offset >= u32BufLen)
        {
            i++;
            break;
        }
        i++;
    }

    if (i == *pu32Parsed)
    {
        return HI_FAILURE;
    }

    *pu32Parsed = i;

    return HI_SUCCESS;
}

#ifdef DMX_USE_ECM
static HI_U32 DMXOsiGetChnSwFlag(HI_U32 ChanId)
{
    DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[ChanId];

    if (ChanInfo->DmxId >= DMX_CNT)
    {
        return 0;
    }

    return ChanInfo->u32SwFlag;
}
#endif

static HI_S32 hi_dmx_irq(int irq, void* devId)
{
    DMX_DEV_OSI_S  *pDmxDevOsi = g_pDmxDevOsi;
    DMX_OQ_Info_S  *OqInfo;
    HI_U32          DmxId;
    HI_U32          OqId;
    HI_U32          ChanId = 0;
    HI_U32          PortId;
    HI_U32          IntStatus;
    HI_U32          i;
    HI_U32          j;

    DmxHalDisableAllPVRInt();

    if (DmxHalGetTotalTeiIntStatus())
    {
        DmxHalGetTeiIntInfo(&DmxId, &ChanId);
        DmxHalClrTeiIntStatus();
        s_u32DMXTEIErr[ChanId]++;
    }

    if (DmxHalGetTotalPcrIntStatus())
    {
        IntStatus = DmxHalGetPcrIntStatus();

        for (i = 0; i < DMX_PCR_CHANNEL_CNT; i++)
        {
            DMX_PCR_Info_S *PcrInfo;

            if (0 == (IntStatus & (1 << i)))
            {
                continue;
            }

            PcrInfo = &pDmxDevOsi->DmxPcrInfo[i];
            if (PcrInfo->DmxId < DMX_CNT)
            {
                DmxHalGetPcrValue(i, &PcrInfo->PcrValue);
                DmxHalGetScrValue(i, &PcrInfo->ScrValue);

            #ifndef MINI_SYS_SURPORT
                if ((DMX_INVALID_SYNC_HANDLE != PcrInfo->SyncHandle) && g_pSyncFunc && g_pSyncFunc->pfnSYNC_PcrProc)
                {
                    HI_U32 Pcr1Khz = 0;

                    /* dmx pcr is sample with the freq of 90khz, but the sync module need the freq of 1khz
                     * so divide 90 here
                     */
                    Pcr1Khz = (HI_U32)((PcrInfo->PcrValue >> 1) & 0xffffffff);  // divide 2
                    Pcr1Khz /= 45;

                    (g_pSyncFunc->pfnSYNC_PcrProc)(PcrInfo->SyncHandle, Pcr1Khz);
                }
            #endif
            }

            DmxHalClrPcrIntStatus(i);
        }
    }

    if (DmxHalGetTotalDiscIntStatus())
    {
        for (i = 0; i < DMX_CHANNEL_REGION_NUM; i++)
        {
            IntStatus = DmxHalGetDiscIntStatus(i);
            if (IntStatus)
            {
                for (j = 0; j < DMX_CHANNEL_NUM_PER_REGION; j++)
                {
                    if (IntStatus & (1 << j))
                    {
                        ChanId = i * DMX_CHANNEL_NUM_PER_REGION + j;

                        DmxHalClearDiscIntStatus(ChanId);
                        s_u32DMXCCDiscErr[ChanId]++;
                    }
                }
            }
        }
    }

    if (DmxHalGetTotalCrcIntStatus())
    {
        for (i = 0; i < DMX_CHANNEL_REGION_NUM; i++)
        {
            IntStatus = DmxHalGetCrcIntStatus(i);
            if (IntStatus)
            {
                for (j = 0; j < DMX_CHANNEL_NUM_PER_REGION; j++)
                {
                    if (IntStatus & (1 << j))
                    {
                        ChanId = i * DMX_CHANNEL_NUM_PER_REGION + j;

                        DmxHalClearCrcIntStatus(ChanId);
                        s_u32DMXCRCErr[ChanId]++;
                    }
                }
            }
        }
    }

    if (DmxHalGetTotalPenLenIntStatus())
    {
        for (i = 0; i < DMX_CHANNEL_REGION_NUM; i++)
        {
            IntStatus = DmxHalGetPesLenIntStatus(i);
            if (IntStatus)
            {
                for (j = 0; j < DMX_CHANNEL_NUM_PER_REGION; j++)
                {
                    if (IntStatus & (1 << j))
                    {
                        ChanId = i * DMX_CHANNEL_NUM_PER_REGION + j;

                        DmxHalClearPesLenIntStatus(ChanId);
                        s_u32DMXPesLenErr[ChanId]++;
                    }
                }
            }
        }
    }

    IntStatus = DmxHalFQGetAllOverflowIntStatus();
    if (IntStatus)
    {
        HI_U32 FqId;

        for (i = 0; i < DMX_FQ_REGION_NUM; i++)
        {
            FqId = i * DMX_FQ_NUM_PER_REGION;

            if (IntStatus & (1 << i))
            {
                HI_U32 FqIntStatus;
                HI_U32 FqIntType;

                FqIntStatus = DmxHalFQGetOverflowIntStatus(i);
                FqIntType = DmxHalFQGetOverflowIntType(i);

                for (j = 0; (j < DMX_FQ_NUM_PER_REGION) && FqIntStatus && (FqId < DMX_FQ_CNT); j++, FqId++)
                {
                    if (0 == (FqIntStatus & (1 << j)))
                    {
                        continue;
                    }

                    FqIntStatus &= ~(1 << j);

                    /*  1:overflow, 0:almost overflow. only record overflow int, usually we not care about "almost overflow". */
                    if ( FqIntType & (1 << j) )
                    {
                        pDmxDevOsi->DmxFqInfo[FqId].FqOverflowCount++;
                    }

                    DmxHalFQSetOverflowInt(FqId, DMX_DISABLE);

                    DmxHalFQClearOverflowInt(FqId);
                }
            }
        }
    }

    IntStatus = DmxHalOQGetAllOverflowIntStatus();
    if (IntStatus)
    {
        for (i = 0; i < DMX_OQ_REGION_NUM; i++)
        {
            if (IntStatus & (1 << i))
            {
                for (j = 0; j < DMX_OQ_NUM_PER_REGION; j++)
                {
                    OqId = i * DMX_OQ_NUM_PER_REGION + j;

                    if (DmxHalOQGetOverflowIntStatus(OqId))
                    {
                        DmxHalOQClearOverflowInt(OqId);
                    }
                }
            }
        }
    }

    IntStatus = DmxHalOQGetAllEopIntStatus();
    if (IntStatus)
    {
        for (i = 0; i < DMX_OQ_REGION_NUM; i++)
        {
            if (0 == (IntStatus & (1 << i)))
            {
                continue;
            }

            for (j = 0; j < DMX_OQ_NUM_PER_REGION; j++)
            {
                OqId = i * DMX_OQ_NUM_PER_REGION + j;

                if (!DmxHalGetOQEopIntStatus(OqId))
                {
                    continue;
                }

                DmxHalClearOQEopIntStatus(OqId);

                OqInfo = &pDmxDevOsi->DmxOqInfo[OqId];

                wake_up_interruptible(&g_pDmxDevOsi->DmxWaitQueue);
                s_u32DMXBufEop[OqId] = 1;

                wake_up_interruptible(&OqInfo->OqWaitQueue);

                if (OqInfo->pWatchWaitQueue || s_pAllChnWatchWaitQueue)
                {
                    ChanId = OqId;
                    if (ChanId < DMX_CHANNEL_CNT)
                    {
                        HI_U32 u32DataFlag;
                        #ifdef DMX_USE_ECM
                        if (DMXOsiGetChnSwFlag(ChanId))
                        {
                            u32DataFlag = HI_DMX_SwGetChannelDataStatus(ChanId);
                        }
                        else
                        {
                        #endif
                            u32DataFlag = DMX_OsiGetChnDataFlag(ChanId);
                        #ifdef DMX_USE_ECM
                        }
                        #endif
                        if (u32DataFlag)
                        {
                            s_u32DMXBufData = 1;
                            if (OqInfo->pWatchWaitQueue)
                            {
                                wake_up_interruptible(OqInfo->pWatchWaitQueue);
                            }
                            else if (s_pAllChnWatchWaitQueue)
                            {
                                wake_up_interruptible(s_pAllChnWatchWaitQueue);
                            }
                        }
                    }
                }
            }
        }
    }

    IntStatus = DmxHalOQGetAllOutputIntStatus();

    for (i = 0; (i < DMX_OQ_REGION_NUM) && IntStatus; i++)
    {
        HI_U32 OqOutInt;

        if (0 == (IntStatus & (1 << i)))
        {
            continue;
        }

        IntStatus &= ~(1 << i);

        OqOutInt = DmxHalOQGetOutputIntStatus(i);

        for (j = 0; (j < DMX_OQ_NUM_PER_REGION) && OqOutInt; j++)
        {
            DMX_OQ_Info_S *OqInfo;

            if (0 == (OqOutInt & (1 << j)))
            {
                continue;
            }

            OqOutInt &= ~(1 << j);

            OqId = i * DMX_OQ_NUM_PER_REGION + j;

            DmxHalOQEnableOutputInt(OqId, HI_FALSE);

            OqInfo = &pDmxDevOsi->DmxOqInfo[OqId];
            if (OqInfo)
            {
                OqInfo->OqWakeUp = HI_TRUE;

                wake_up_interruptible(&OqInfo->OqWaitQueue);
            }
        }
    }

    for (PortId = 0; PortId < DMX_RAMPORT_CNT; PortId++)
    {
        if (DmxHalIPPortGetOutIntStatus(PortId))
        {
            DMX_RamPort_Info_S *PortInfo = HI_NULL;
            HI_SIZE_T           LockFlag;

            DmxHalIPPortClearOutIntStatus(PortId);

            PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];
            
            spin_lock_irqsave(&PortInfo->LockRamPort, LockFlag);
            if (0 != PortInfo->Write)
            {
                HI_U32  DescRead;
                HI_U32 *ReadAddr;

                DescRead = DmxHalIPPortDescGetRead(PortId);
                if (0 == DescRead)
                {
                    DescRead = PortInfo->DescDepth - 1;
                }
                else
                {
                    --DescRead;
                }

                ReadAddr = (HI_U32*)(PortInfo->DescKerAddr + (DescRead << 4));

                PortInfo->Read = *ReadAddr - PortInfo->PhyAddr;
                if (PortInfo->Read == PortInfo->Write)
                {
                    PortInfo->Read  = 0;
                    PortInfo->Write = 0;
                }
            }
            spin_unlock_irqrestore(&PortInfo->LockRamPort, LockFlag);

            if (PortInfo->WaitLen)
            {
                if (!CheckIPBuf(PortId, PortInfo->WaitLen))
                {
                    PortInfo->WakeUp    = HI_TRUE;
                    PortInfo->WaitLen   = 0;

                    wake_up_interruptible(&PortInfo->WaitQueue);
                }
            }
        }
    }

    DmxHalEnableAllPVRInt();

    return IRQ_HANDLED;
}

static HI_VOID DMXOsiGetDataFlag(HI_U32 *pu32WatchCh, HI_U32 u32WatchNum, HI_U32 *pu32Flag)
{
    HI_U32 i;
    HI_U32 ChanId;

    for (i = 0; i < u32WatchNum; i++)
    {
        ChanId = pu32WatchCh[i];
    #ifdef DMX_USE_ECM
        if (DMXOsiGetChnSwFlag(ChanId))
        {
            if (HI_DMX_SwGetChannelDataStatus(ChanId))
            {
                pu32Flag[ChanId >> 5] |= 1 << (ChanId & 0x1F);
            }

            continue;
        }
    #endif

        if (DMX_OsiGetChnDataFlag(ChanId))
        {
            pu32Flag[ChanId >> 5] |= 1 << (ChanId & 0x1F);
        }
    }
}

static HI_S32 DMXOsiChannelWaitTimeOut(DMX_ChanInfo_S *ChanInfo, HI_U32 u32TimeOutMs)
{
    HI_S32          ret;
    DMX_OQ_Info_S  *OqInfo  = &g_pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

    s_u32DMXBufEop[ChanInfo->ChanOqId] = 0;

    DmxHalEnableOQEopInt(ChanInfo->ChanOqId);
    ret = wait_event_interruptible_timeout(OqInfo->OqWaitQueue,
                                           (s_u32DMXBufEop[ChanInfo->ChanOqId]),
                                           msecs_to_jiffies(u32TimeOutMs));
    DmxHalDisableOQEopInt(ChanInfo->ChanOqId);
    return ret;
}

/**
 \brief

 \attention

 \param[in] pChanInfo
 \param[in] u32AcqNum
 \param[out] pu32AcqedNum
 \param[out] psMsgList
 \param[in] u32TimeOutMs

 \retval none
 \return none

 \see
 \li ::
 */
static HI_S32 DMXOsiReadPes(DMX_ChanInfo_S * pChanInfo,
                            HI_U32         u32AcqNum,
                            HI_U32 *       pu32AcqedNum,
                            DMX_UserMsg_S* psMsgList,
                            HI_U32         u32TimeOutMs)
{
    HI_S32 ret;

    u32TimeOutMs = (u32TimeOutMs * HZ) / 1000;

    ret = DMXOsiFindPes(pChanInfo, psMsgList, u32AcqNum, pu32AcqedNum);
    if (ret == HI_SUCCESS)
    {
        pChanInfo->u32HitAcq++;
        return ret;
    }

    if (u32TimeOutMs)
    {
        ret = DMXOsiChannelWaitTimeOut(pChanInfo, u32TimeOutMs);
        if (ret != 0)
        {
            ret = DMXOsiFindPes(pChanInfo, psMsgList, u32AcqNum, pu32AcqedNum);
            if (ret == HI_SUCCESS)
            {
                pChanInfo->u32HitAcq++;
                return ret;
            }
        }
        else
        {
            *pu32AcqedNum = 0;
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }
    }

    //no availibale data
    *pu32AcqedNum = 0;
    return HI_ERR_DMX_NOAVAILABLE_DATA;
}

static HI_S32 DMXOsiGetSecNum(DMX_ChanInfo_S * pChanInfo,
                              HI_U32*        pu32BlkNum,
                              HI_U32         u32AcqNum,
                              DMX_UserMsg_S* psMsgList,
                              DMX_OQ_Info_S* OqInfo)
{
    HI_U32 u32CurrentBlk, u32BufPhyAddr, u32DataLen;
    HI_U32 u32Offset;
    HI_U32 u32WriteBlk, u32ReadBlk, u32ProcsBlk;
    HI_S32 s32Ret;
    OQ_DescInfo_S  *oq_dsc;

    DMXOsiOQGetReadWrite(pChanInfo->ChanOqId, &u32WriteBlk, HI_NULL);

    u32Offset   = OqInfo->u32ProcsOffset;
    u32ReadBlk  = OqInfo->u32ProcsBlk;
    u32ProcsBlk = u32ReadBlk;
    *pu32BlkNum = 0;
    while (u32ReadBlk != u32WriteBlk)
    {
        u32CurrentBlk = OqInfo->u32OQVirAddr + u32ReadBlk * DMX_OQ_DESC_SIZE;
        oq_dsc = (OQ_DescInfo_S*)u32CurrentBlk;
        if (!oq_dsc)
        {
            HI_ERR_DEMUX("DMXOsiGetSecNum oqdsc error,check it!!!\n");
            break;
        }

        u32BufPhyAddr = oq_dsc->start_addr;
        u32DataLen = oq_dsc->pvrctrl_datalen & 0xffff;
        if (u32Offset >= u32DataLen)
        {
            DMXINC(u32ReadBlk, OqInfo->u32OQDepth);
            DMXINC(u32ProcsBlk, OqInfo->u32OQDepth);
            OqInfo->u32ProcsBlk = u32ProcsBlk;
            OqInfo->u32ProcsOffset = 0;
            u32Offset = 0;
            continue;
        }

        s32Ret = DMXOsiParseSection(pu32BlkNum, u32AcqNum, &u32Offset, u32BufPhyAddr, u32DataLen, pChanInfo, psMsgList, OqInfo);
        if (s32Ret != HI_SUCCESS)
        {
            continue;
        }
        else
        {
            if (u32Offset >= u32DataLen)
            {
                DMXINC(u32ProcsBlk, OqInfo->u32OQDepth);
                u32Offset = 0;
            }

            if (*pu32BlkNum >= u32AcqNum)
            {
                break;
            }
        }

        DMXINC(u32ReadBlk, OqInfo->u32OQDepth);
    }

    if (*pu32BlkNum > 0)
    {
        OqInfo->u32ProcsBlk = u32ProcsBlk;
        OqInfo->u32ProcsOffset = u32Offset;
    }

    return HI_SUCCESS;
}

static HI_U32 DMXOsiGetCurEopaddr(DMX_ChanInfo_S *ChanInfo)
{
    HI_U32 EopAddr = 0;
    HI_U32 PvrCtrl;

    DmxHalGetOQWORDx(ChanInfo->ChanOqId, DMX_OQ_CTRL_OFFSET, &PvrCtrl);

    if (PvrCtrl & DMX_MASK_BIT_7)
    {
        HI_U32 Value;

        DmxHalGetOQWORDx(ChanInfo->ChanOqId, DMX_OQ_EOPWR_OFFSET, &Value);

        if (HI_UNF_DMX_CHAN_TYPE_POST != ChanInfo->ChanType)
        {
            EopAddr = Value >> DMX_SHIFT_16BIT;
        }
        else
        {
            EopAddr = Value & 0xffff;
        }

#ifdef DMX_USE_ECM
        if (ChanInfo->u32SwFlag)
        {
            EopAddr = Value & 0xffff;
        }
#endif
    }

    return EopAddr;
}

static HI_S32 DMXOsiGetCurSAddr(DMX_ChanInfo_S *pChanInfo, HI_U32* pu32BufPhyAddr, HI_U32* pu32EopAddr,DMX_OQ_Info_S  *OqInfo)
{
    HI_U32 u32ReadBlk, u32WriteBlk;
    HI_U32 u32OQId;

    u32OQId = pChanInfo->ChanOqId;
    u32ReadBlk   = OqInfo->u32ProcsBlk;
    *pu32EopAddr = DMXOsiGetCurEopaddr(pChanInfo);
    DmxHalGetOQWORDx(u32OQId, DMX_OQ_SADDR_OFFSET, pu32BufPhyAddr);
    DMXOsiOQGetReadWrite(u32OQId, &u32WriteBlk, HI_NULL);

    if (u32WriteBlk == u32ReadBlk)
    {
        return HI_SUCCESS;
    }

    return HI_FAILURE;
}

static HI_S32 DmxOsiReadToMsgBuffer(DMX_ChanInfo_S *pChanInfo, HI_U32 u32AcqNum,
                                    HI_U32 *pu32AcqedNum, DMX_UserMsg_S* psMsgList,DMX_OQ_Info_S* OqInfo)
{
    HI_U32 u32BlkNum = 0;
    HI_S32 s32Ret;

    s32Ret = DMXOsiGetSecNum(pChanInfo, &u32BlkNum, u32AcqNum, psMsgList,OqInfo);
    if ((HI_SUCCESS == s32Ret) && (u32BlkNum > 0))
    {
        *pu32AcqedNum = u32BlkNum;
        return HI_SUCCESS;
    }

    *pu32AcqedNum = 0;

    return HI_ERR_DMX_NOAVAILABLE_DATA;
}

static HI_S32 DMXOsiReadSec(DMX_ChanInfo_S * pChanInfo,
                            HI_U32         u32AcqNum,
                            HI_U32 *       pu32AcqedNum,
                            DMX_UserMsg_S* psMsgList,
                            DMX_OQ_Info_S* OqInfo,
                            HI_U32         u32TimeOutMs)
{
    HI_S32 ret;
    HI_U32 u32BufPhyAddr;
    HI_U32* pu32Offset;
    HI_U32 u32EopAddr;
    HI_U32 u32BlkNum = 0;    

    u32EopAddr = DMXOsiGetCurEopaddr(pChanInfo);

    ret = DmxOsiReadToMsgBuffer(pChanInfo, u32AcqNum, pu32AcqedNum, psMsgList,OqInfo);
    if ((ret == HI_SUCCESS) && (*pu32AcqedNum > 0))
    {
    #ifdef DMX_USE_ECM
        if (!pChanInfo->u32SwFlag)
    #endif
        {
            pChanInfo->u32HitAcq++;
        }

        return HI_SUCCESS;
    }

    pu32Offset = &OqInfo->u32ProcsOffset;
    if (*pu32Offset == u32EopAddr)
    {
        if (!u32TimeOutMs)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        ret = DMXOsiChannelWaitTimeOut(pChanInfo, u32TimeOutMs);
        if (0 == ret)
        {
            HI_WARN_DEMUX("OQ %d wait time out!\n", pChanInfo->ChanOqId);

            return HI_ERR_DMX_TIMEOUT;
        }

        ret = DmxOsiReadToMsgBuffer(pChanInfo, u32AcqNum, pu32AcqedNum, psMsgList,OqInfo);
        if (ret == HI_SUCCESS)
        {
        #ifdef DMX_USE_ECM
            if (!pChanInfo->u32SwFlag)
        #endif
            {
                pChanInfo->u32HitAcq++;
            }

            return HI_SUCCESS;
        }
    }

    if (DMXOsiGetCurSAddr(pChanInfo, &u32BufPhyAddr, &u32EopAddr,OqInfo) != HI_SUCCESS)
    {
        ret = DmxOsiReadToMsgBuffer(pChanInfo, u32AcqNum, pu32AcqedNum, psMsgList,OqInfo);
        if (ret == HI_SUCCESS)
        {
        #ifdef DMX_USE_ECM
            if (!pChanInfo->u32SwFlag)
        #endif
            {
                pChanInfo->u32HitAcq++;
            }

            return HI_SUCCESS;
        }

        //read eop again,if not success return
        if (DMXOsiGetCurSAddr(pChanInfo, &u32BufPhyAddr, &u32EopAddr,OqInfo) != HI_SUCCESS)
        {
            HI_WARN_DEMUX("OQ %d no new eop with eop interrupt,rec addr:%x ,eop addr :%x!\n",
                          pChanInfo->ChanOqId, *pu32Offset, u32EopAddr);

            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }
    }

    DMXOsiParseSection(&u32BlkNum, u32AcqNum, pu32Offset, u32BufPhyAddr, u32EopAddr, pChanInfo, psMsgList, OqInfo);

    if (0 == u32BlkNum)
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }

#ifdef DMX_USE_ECM
    if (!pChanInfo->u32SwFlag)
#endif
    {
        pChanInfo->u32HitAcq++;
    }

    *pu32AcqedNum = u32BlkNum;

    return HI_SUCCESS;
}

static HI_U32 FilterGetValidDepth(const HI_UNF_DMX_FILTER_ATTR_S *FilterAttr)
{
    HI_U32 Depth = FilterAttr->u32FilterDepth;

    while (Depth)
    {
        if (0xff == FilterAttr->au8Mask[Depth - 1])
        {
            --Depth;
        }
        else
        {
            break;
        }
    }

    return Depth;
}

/**
\brief get channel buffer status
\attention
none
\param[in] u32ChanId

\retval none
\return 0  channel can't (or no need to)  reset
        1  channel buffer is < 20% of pool buffer,and it's buffer is > 1
        2  channel buffer is > 20% of pool buffer
\see
\li ::
*/
static HI_S32 GetChnDataStatus(HI_U32 ChanId)
{
    HI_U32 u32OqId, u32OqRead, u32OqWrite,u32FQId;
    HI_U32 u32FqDepth,u32ValDes;
    DMX_DEV_OSI_S  *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_OQ_Info_S  *OqInfo      = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

    if ((ChanInfo->DmxId >= DMX_CNT) || OqInfo->u32FQId != DMX_FQ_COMMOM)
    {
        return 0; //not usded channel,and not common channel,do not process
    }

    u32OqId = ChanInfo->ChanOqId;
    DMXOsiOQGetReadWrite(u32OqId, &u32OqWrite, &u32OqRead);

    if (OqInfo->u32ProcsBlk != u32OqRead)//there's buffer not released
    {
        return 0;
    }

    u32FQId = OqInfo->u32FQId;
    u32FqDepth = pDmxDevOsi->DmxFqInfo[u32FQId].u32FQDepth;
    u32ValDes = GetQueueLenth(u32OqRead, u32OqWrite, OqInfo->u32OQDepth);
    if(u32ValDes > (u32FqDepth*DMX_CHN_RESET_PERSENT/100))// > 20% of pool buffer
    {
        return 2;
    }

    if (u32ValDes > 1)//have buffer more than 1 block
    {
        return 1;
    }

    HI_DRV_SYS_GetTimeStampMs(&ChanInfo->u32AcqTime);   //if channel buffer is < 1,flush the acquire time

    return 0;
}
/**
\brief get channel acquire time status
\attention
none
\param[in] u32ChanId

\retval none
\return 0: do not care
        1: last acquire time - now > 2s
        2: last acquire time - now > 5s

\see
\li ::
*/
static HI_S32 GetChnAcqTimeStatus(HI_U32 ChanId)
{
    DMX_DEV_OSI_S  *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_OQ_Info_S  *OqInfo      = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
    HI_U32 u32AcqTime,u32TimeNow;

    if ((ChanInfo->DmxId >= DMX_CNT) || OqInfo->u32FQId != DMX_FQ_COMMOM)
    {
        return 0; //not usded channel,and not common channel,do not process
    }

    HI_DRV_SYS_GetTimeStampMs(&u32TimeNow);

    u32TimeNow += 10;   //add 10ms, void situation 1999ms < 2000

    u32AcqTime = ChanInfo->u32AcqTime;
    if ((u32TimeNow - u32AcqTime) > DMX_RESETCHN_TIME2)// > 5s
    {
        return 2;
    }

    if ((u32TimeNow - u32AcqTime) > DMX_RESETCHN_TIME1)// > 2s
    {
        return 1;
    }
    return 0;
}
/**
\brief get fq status
\attention
none
\param[in] u32FQId

\retval none
\return 0:do not care
        1: fq used des > 80% of fq buffer size

\see
\li ::
*/
static HI_S32 GetFQStatus(HI_U32 u32FQId)
{
    HI_U32 FQ_WPtr, FQ_RPtr, ValidDes,UsedDes,tmp;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;

    DmxHalGetFQWORDx(u32FQId, DMX_FQ_SZWR_OFFSET, &tmp);
    FQ_WPtr = tmp & 0xffff;
    DmxHalGetFQWORDx(u32FQId, DMX_FQ_RDVD_OFFSET, &tmp);
    FQ_RPtr = tmp & 0xffff;

    if (FQ_WPtr >= FQ_RPtr)
    {
        ValidDes = FQ_WPtr - FQ_RPtr;
    }
    else
    {
        ValidDes = (pDmxDevOsi->DmxFqInfo[u32FQId].u32FQDepth) - FQ_RPtr + FQ_WPtr;
    }
    UsedDes = pDmxDevOsi->DmxFqInfo[u32FQId].u32FQDepth - ValidDes;

    if (UsedDes >= (pDmxDevOsi->DmxFqInfo[u32FQId].u32FQDepth)* DMX_CHECK_START_PERSENT/ 100)
    {
       return 1;
    }

    return 0;
}

static  HI_VOID ResetPoolBuf(HI_VOID)
{
    HI_U32 i = 0;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S*  pstDmxChn;
    HI_SIZE_T           LockFlag;

    spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), LockFlag);
    DmxFqStop(DMX_FQ_COMMOM);
    
    for ( i = 0 ; i < DMX_CHANNEL_CNT; i++ )
    {
        pstDmxChn = &pDmxDevOsi->DmxChanInfo[i];
        if ((pstDmxChn->DmxId < DMX_CNT) && ((pstDmxChn->ChanOutMode & HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY) == HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY))
        {
            if ( (pstDmxChn->ChanType == HI_UNF_DMX_CHAN_TYPE_SEC) ||
                (pstDmxChn->ChanType == HI_UNF_DMX_CHAN_TYPE_PES) ||
                (pstDmxChn->ChanType == HI_UNF_DMX_CHAN_TYPE_POST) ) 
            {
                DmxOqStop(pstDmxChn->ChanOqId);
                DmxFlushChannel(pstDmxChn->ChanId, DMX_FLUSH_TYPE_PLAY);
                DmxFlushOq(pstDmxChn->ChanOqId, DMX_OQ_CLEAR_TYPE_PLAY);
                DmxOqStart(pstDmxChn->ChanOqId);
            }
        }

    }
    
    DmxFqStart(DMX_FQ_COMMOM);
    spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), LockFlag);
}


HI_VOID CheckChnTimeoutProc(HI_LENGTH_T TimerPara)
{
    HI_S32 s32Ret;
    HI_U32 i;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S*  pstDmxChn;

    //check all channel
    if (GetFQStatus(DMX_FQ_COMMOM))//dmx pool buffer used > 80%
    {
        for ( i = 0 ; i < DMX_CHANNEL_CNT; i++ )
        {
            pstDmxChn = &pDmxDevOsi->DmxChanInfo[i];
            if (pstDmxChn->u32ChnResetLock)
            {
                continue;
            }

            s32Ret = GetChnDataStatus(i);
            if (s32Ret == 2)//channel buffer > 20% of dmx pool buffer
            {
                if (GetChnAcqTimeStatus(i) >= 1)
                {
                    DMX_OsiResetChannel(i, DMX_FLUSH_TYPE_REC_PLAY);//reset channel
                    HI_DRV_SYS_GetTimeStampMs(&pstDmxChn->u32AcqTime);

                    pDmxDevOsi->DmxFqInfo[DMX_FQ_COMMOM].FqOverflowCount++;
                }
            }
            else if (s32Ret == 1)//channel buffer < 20% of dmx pool buffer,> 1 blk
            {
                if (GetChnAcqTimeStatus(i) == 2)// > 5s not receive data
                {
                    DMX_OsiResetChannel(i, DMX_FLUSH_TYPE_REC_PLAY);//reset channel
                    HI_DRV_SYS_GetTimeStampMs(&pstDmxChn->u32AcqTime);

                    pDmxDevOsi->DmxFqInfo[DMX_FQ_COMMOM].FqOverflowCount++;
                }
            }
        }
    }

    mod_timer(&CheckChnTimeoutTimer, jiffies + msecs_to_jiffies(DMX_CHECKCHN_TIMEOUT));
    
    return;
}

#ifdef CHIP_TYPE_hi3716m
static HI_VOID DmxGetHi3716MV300Flag(HI_VOID)
{
     HI_CHIP_VERSION_E Version;

     HI_DRV_SYS_GetChipVersion(HI_NULL, &Version);

     Hi3716MV300Flag = (HI_CHIP_VERSION_V300 == Version) ? HI_TRUE : HI_FALSE;
}
#endif

static HI_S32 DmxReset(HI_VOID)
{
    HI_S32 ret;

    DmxHalConfigHardware();

    udelay(10);

    ret = DmxHalGetInitStatus();
    if (HI_SUCCESS != ret)
    {
        HI_FATAL_DEMUX("status error\n");

        return ret;
    }

    DmxHalIPPortAutoClearBP();

#ifdef DMX_FILTER_DEPTH_SUPPORT
    DmxHalFilterEnableDepth();
#endif

#ifdef CHIP_TYPE_hi3716m
    DmxGetHi3716MV300Flag();

    if (Hi3716MV300Flag)
    {
        DmxHalFilterSetSecStuffCtrl(HI_TRUE);
    }
#else
    #ifdef DMX_SECTION_PARSE_NOPUSI_SUPPORT
    DmxHalFilterSetSecStuffCtrl(HI_TRUE);
    #endif
#endif

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiInit
* Description   : Initialize demux module
* Input         :
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:     system error or allocated dma buffer size beyonds limit
* Others:
***********************************************************************************/
HI_S32 DMX_OsiInit(HI_U32 PoolBufSize, HI_U32 BlockSize)
{
    DMX_DEV_OSI_S          *pDmxDevOsi;
    HI_UNF_DMX_PORT_ATTR_S  PortAttr;
    DMX_FQ_Info_S          *FqInfo;
    HI_U32                  BufSize;
    HI_U32                  FqDepth;
    HI_U32                  FqBufSize;
    MMZ_BUFFER_S            MmzBuf;
    HI_U32                  i;
    HI_UNF_DMX_TSO_PORT_ATTR_S  TSOPortAttr;

    if (HI_NULL != g_pDmxDevOsi)
    {
        return HI_SUCCESS;
    }

    if (HI_SUCCESS != DmxReset())
    {
        return HI_ERR_DMX_BUSY;
    }

    BufSize     = (PoolBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;
    FqDepth     = BufSize / (BlockSize + DMX_BUF_INTERVAL) + 1;
    FqBufSize   = FqDepth * DMX_FQ_DESC_SIZE;

    if (HI_SUCCESS != HI_DRV_MMZ_AllocAndMap("DMX_PoolBuf", MMZ_OTHERS, BufSize + FqBufSize, 0, &MmzBuf))
    {
        HI_FATAL_DEMUX("memory allocate failed\n");

        return HI_ERR_DMX_ALLOC_MEM_FAILED;
    }

    pDmxDevOsi = HI_KMALLOC(HI_ID_DEMUX, sizeof(DMX_DEV_OSI_S), GFP_KERNEL);
    if (HI_NULL == pDmxDevOsi)
    {
        HI_FATAL_DEMUX("memory allocate failed\n");

        HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);

        return HI_ERR_DMX_ALLOC_MEM_FAILED;
    }

    g_pDmxDevOsi = pDmxDevOsi;

    memset((HI_VOID*)pDmxDevOsi, 0, sizeof(DMX_DEV_OSI_S));

    mutex_init(&pDmxDevOsi->lock_Channel);
    mutex_init(&pDmxDevOsi->lock_Filter);
    mutex_init(&pDmxDevOsi->lock_Key);
    mutex_init(&pDmxDevOsi->lock_AVChan);

    spin_lock_init(&pDmxDevOsi->splock_OqBuf);

    FqInfo = &pDmxDevOsi->DmxFqInfo[DMX_FQ_COMMOM];

    FqInfo->u32IsUsed       = 1;
    FqInfo->u32BufPhyAddr   = MmzBuf.u32StartPhyAddr;
    FqInfo->u32BufVirAddr   = MmzBuf.u32StartVirAddr;
    FqInfo->u32BufSize      = BufSize;
    FqInfo->u32BlockSize    = BlockSize;
    FqInfo->u32FQDepth      = FqDepth;
    FqInfo->u32FQPhyAddr    = FqInfo->u32BufPhyAddr + BufSize;
    FqInfo->u32FQVirAddr    = FqInfo->u32BufVirAddr + BufSize;

    DmxFqStart(DMX_FQ_COMMOM);

    for (i = 0; i < DMX_TUNERPORT_CNT; i++)
    {
        PortAttr.enPortMod              = HI_UNF_DMX_PORT_MODE_EXTERNAL;
        PortAttr.enPortType             = HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188;
        PortAttr.u32SyncLostTh          = 1;
        PortAttr.u32SyncLockTh          = 5;
        PortAttr.u32TunerInClk          = 0;
        PortAttr.u32SerialBitSelector   = 0;
        PortAttr.u32TunerErrMod         = 0;
        PortAttr.u32UserDefLen1         = 0;
        PortAttr.u32UserDefLen2         = 0;

        DMX_OsiTunerPortSetAttr(i, &PortAttr);

        DmxHalDvbPortSetTsCountCtrl(i, TS_COUNT_CRTL_START);
        DmxHalDvbPortSetErrTsCountCtrl(i, TS_COUNT_CRTL_START);
    }

    for (i = 0; i < DMX_RAMPORT_CNT; i++)
    {
        DMX_RamPort_Info_S *PortInfo = &pDmxDevOsi->RamPortInfo[i];

        init_waitqueue_head(&PortInfo->WaitQueue);

        PortAttr.enPortMod              = HI_UNF_DMX_PORT_MODE_RAM;
    #ifdef DMX_RAM_PORT_AUTO_SCAN_SUPPORT
        PortAttr.enPortType             = HI_UNF_DMX_PORT_TYPE_AUTO;
    #else
        PortAttr.enPortType             = HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188_204;
    #endif
        PortAttr.u32SyncLostTh          = 0;
        PortAttr.u32SyncLockTh          = 7;
        PortAttr.u32TunerInClk          = 0;
        PortAttr.u32SerialBitSelector   = 0;
        PortAttr.u32TunerErrMod         = 0;
        PortAttr.u32UserDefLen1         = 0;
        PortAttr.u32UserDefLen2         = 0;

        DMX_OsiRamPortSetAttr(i, &PortAttr);

        DmxHalIPPortEnableInt(i);
        DmxHalIPPortSetIntCnt(i, DMX_DEFAULT_INT_CNT);

        DmxHalIPPortSetTsCountCtrl(i, HI_TRUE);
    }

    for ( i = 0 ; i < DMX_TSOPORT_CNT; i++ )
    {
        TSOPortAttr.bEnable         = HI_TRUE;
        TSOPortAttr.bClkReverse     = HI_TRUE;
        TSOPortAttr.enTSSource      = 0; /* 0: no Source */
        TSOPortAttr.enClkMode       = HI_UNF_DMX_TSO_CLK_MODE_NORMAL;
        TSOPortAttr.enValidMode     = HI_UNF_DMX_TSO_VALID_ACTIVE_OUTPUT;
        TSOPortAttr.bBitSync        = HI_TRUE;
        TSOPortAttr.bSerial         = HI_TRUE;
        TSOPortAttr.bLSB            = HI_FALSE;

        DmxHalGetTSOClkCfg(i,&TSOPortAttr.bClkReverse,(HI_U32*)&TSOPortAttr.enClk,&TSOPortAttr.u32ClkDiv);
        DMX_OsiSetTSOPortAttr(i, &TSOPortAttr);
    }

    for (i = 0; i < DMX_CNT; i++)
    {
        DMX_Sub_DevInfo_S  *DmxInfo = &pDmxDevOsi->SubDevInfo[i];
        DMX_RecInfo_S      *RecInfo = &pDmxDevOsi->DmxRecInfo[i];

        DmxInfo->PortMode   = DMX_PORT_MODE_BUTT;
        DmxInfo->PortId     = DMX_INVALID_PORT_ID;

        RecInfo->DmxId      = DMX_INVALID_DEMUX_ID;
        
        INIT_LIST_HEAD(&RecInfo->head);   
        RecInfo->RemIdxCnt = 0;

        mutex_init(&RecInfo->LockRec);

        DmxHalSetSpsRefRecCh(i, 0xff);
    }

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &pDmxDevOsi->DmxChanInfo[i];

        ChanInfo->DmxId             = DMX_INVALID_DEMUX_ID;
        ChanInfo->ChanPid           = DMX_INVALID_PID;
        ChanInfo->ChanId            = DMX_INVALID_CHAN_ID;
        ChanInfo->KeyId             = DMX_INVALID_KEY_ID;
        ChanInfo->u32ChnResetLock   = 0;
#ifdef HI_DMX_PES_EXT_DATA_SUPPORT
        mutex_init(&(ChanInfo->stDrvPesExtData.pes_lock));
        ChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType = HI_MPI_DMX_PES_EXT_DATA_TYPE_BUTT;
#endif
    #ifdef DMX_USE_ECM
        ChanInfo->u32SwFlag         = 0;
    #endif

        DmxHalSetChannelPlayDmxid(i, DMX_INVALID_DEMUX_ID);
        DmxHalSetChannelRecDmxid(i, DMX_INVALID_DEMUX_ID);
        DmxHalSetChannelPid(i, DMX_INVALID_PID);
    }

    for (i = 0; i < DMX_FILTER_CNT; i++)
    {
        DMX_FilterInfo_S *FilterInfo = &pDmxDevOsi->DmxFilterInfo[i];

        FilterInfo->ChanId      = DMX_INVALID_CHAN_ID;
        FilterInfo->FilterId    = DMX_INVALID_FILTER_ID;
        FilterInfo->Depth       = 0;
    }



    for (i = 0; i < DMX_PCR_CHANNEL_CNT; i++)
    {
        pDmxDevOsi->DmxPcrInfo[i].DmxId = DMX_INVALID_DEMUX_ID;
    }

    for (i = 0; i < DMX_OQ_CNT; i++)
    {
        DMX_OQ_Info_S  *OqInfo = &pDmxDevOsi->DmxOqInfo[i];

        OqInfo->u32OQId     = i;
        OqInfo->OqWakeUp    = HI_FALSE;

        init_waitqueue_head(&OqInfo->OqWaitQueue);
    }

    for (i = 0; i < DMX_FQ_CNT; i++)
    {
        DMX_FQ_Info_S  *FqInfo = &pDmxDevOsi->DmxFqInfo[i];

        spin_lock_init(&FqInfo->LockFq);
    }

    // default global set
    DmxHalEnableAllDavInt();
    DmxHalEnableAllChEopInt();
    DmxHalEnableAllChEnqueInt();
    DmxHalFQEnableAllOverflowInt();

    DmxHalSetFlushMaxWaitTime(0x2400);
    DmxHalSetDataFakeMod(DMX_ENABLE);
    init_waitqueue_head(&pDmxDevOsi->DmxWaitQueue);
#ifdef DMX_USE_ECM
    HI_DMX_SwInit();
#endif
    init_waitqueue_head(&AllChnWaitQueue);
    memset(&psErrMsg, 0, sizeof(DMX_ERRMSG_S) * DMX_MAX_ERRLIST_NUM);

    mod_timer(&CheckChnTimeoutTimer, jiffies + msecs_to_jiffies(DMX_CHECKCHN_TIMEOUT));
    
    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiDeInit
* Description   : destroy demux module
* Input         :
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiDeInit(HI_VOID)
{
    if (g_pDmxDevOsi)
    {
        DMX_FQ_Info_S  *FqInfo = &g_pDmxDevOsi->DmxFqInfo[DMX_FQ_COMMOM];
        MMZ_BUFFER_S    MmzBuf;
        HI_U32          i;

        del_timer_sync(&CheckChnTimeoutTimer);

        for (i = 0; i < DMX_RAMPORT_CNT; i++)
        {
            if (0 != g_pDmxDevOsi->RamPortInfo[i].DescPhyAddr)
            {
                MMZ_BUFFER_S MmzBuf;

                MmzBuf.u32StartPhyAddr = g_pDmxDevOsi->RamPortInfo[i].DescPhyAddr;
                MmzBuf.u32StartVirAddr = g_pDmxDevOsi->RamPortInfo[i].DescKerAddr;

                HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);
            }

            DmxHalIPPortDisableInt(i);
            DmxHalIPPortSetTsCountCtrl(i, HI_FALSE);
        }

        for (i = 0; i < DMX_TUNERPORT_CNT; i++)
        {
            DmxHalDvbPortSetTsCountCtrl(i, TS_COUNT_CRTL_STOP);
            DmxHalDvbPortSetTsCountCtrl(i, TS_COUNT_CRTL_RESET);

            DmxHalDvbPortSetErrTsCountCtrl(i, TS_COUNT_CRTL_STOP);
            DmxHalDvbPortSetErrTsCountCtrl(i, TS_COUNT_CRTL_RESET);
        }

        DmxFqStop(DMX_FQ_COMMOM);

        MmzBuf.u32StartPhyAddr  = FqInfo->u32BufPhyAddr;
        MmzBuf.u32StartVirAddr  = FqInfo->u32BufVirAddr;

        HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);

        HI_KFREE(HI_ID_DEMUX, g_pDmxDevOsi);
        g_pDmxDevOsi = HI_NULL;
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiGetPoolBufAddr(HI_U32 *PhyAddr, HI_U32 *BufSize)
{
    DMX_FQ_Info_S  *FqInfo = &g_pDmxDevOsi->DmxFqInfo[DMX_FQ_COMMOM];

    *PhyAddr = FqInfo->u32BufPhyAddr;
    *BufSize = FqInfo->u32BufSize;

    return HI_SUCCESS;
}

HI_VOID DMX_OsiSetNoPusiEn(HI_BOOL bNoPusiEn)
{
        DmxHalFilterSetSecStuffCtrl(bNoPusiEn);
}

HI_VOID DMX_OsiSetTei(HI_U32 u32DmxId,HI_BOOL bTei)
{
        DmxHalSetTei(u32DmxId,bTei);
}

#ifdef HI_DEMUX_TSO_SUPPORT
HI_S32 DMX_OsiGetTSOPortAttr(const HI_U32 PortId, HI_UNF_DMX_TSO_PORT_ATTR_S *PortAttr)
{
    HI_UNF_DMX_TSO_PORT_ATTR_S *PortInfo = &g_pDmxDevOsi->TSOPortInfo[PortId];

    PortAttr->bEnable         = PortInfo->bEnable;
    PortAttr->bClkReverse     = PortInfo->bClkReverse;
    PortAttr->enTSSource      = PortInfo->enTSSource;
    PortAttr->enClkMode       = PortInfo->enClkMode;
    PortAttr->enValidMode     = PortInfo->enValidMode;
    PortAttr->bBitSync        = PortInfo->bBitSync;
    PortAttr->bSerial         = PortInfo->bSerial;
    PortAttr->bLSB            = PortInfo->bLSB;
    PortAttr->enClk           = PortInfo->enClk;
    PortAttr->u32ClkDiv       = PortInfo->u32ClkDiv;

    return HI_SUCCESS;
}

static HI_VOID DMXConfigIPPortRate(HI_U32 PortId)
{
    HI_U8 i = 0;
    HI_U32 IPRate = DMX_DEFAULT_IP_SPEED;
    HI_U32 TSOClk = 150;
    HI_U32 DmxClk;
    HI_UNF_DMX_TSO_PORT_ATTR_S PortAttr;

    DmxClk = DmxHalGetClk();
	
    /*Attention: now we do not support MV300 TSO */
    for ( i = 0 ; i < DMX_TSOPORT_CNT; i++ )
    {
        DMX_OsiGetTSOPortAttr(i, &PortAttr);
        if ( (PortAttr.enTSSource  == PortId + HI_UNF_DMX_PORT_3_RAM))
        {
            switch ( PortAttr.enClk )
            {
                case HI_UNF_DMX_TSO_CLK_100M :
                    TSOClk = 100;
                    break;
                case HI_UNF_DMX_TSO_CLK_150M :
                    TSOClk = 150;
                    break;
                case HI_UNF_DMX_TSO_CLK_1200M :
                    TSOClk = 1200;
                    break;
                case HI_UNF_DMX_TSO_CLK_1572M :
                    TSOClk = 1572;
                    break;
                case HI_UNF_DMX_TSO_CLK_BUTT :
                    HI_ERR_DEMUX("TSO clk invalid\n");
                    break;
            }

            TSOClk = TSOClk/PortAttr.u32ClkDiv;

            if ( !PortAttr.bSerial )
            {
                TSOClk = TSOClk*8;
            }
            
            /*
            IPClk = DmxClk*8/(IPRate+1) < TSOClk; so IPRate >  DmxClk*8/TSOClk -1 ;
            */
            IPRate =  DmxClk*8/TSOClk ;
			
            /* experiment tune value for external decrease ram port dispatch rate */
            IPRate += 3;
        }
    }
	
    DmxHalIPPortRateSet(PortId, IPRate);
}

HI_S32 DMX_OsiSetTSOPortAttr(const HI_U32 PortId, const HI_UNF_DMX_TSO_PORT_ATTR_S *PortAttr)
{
    HI_UNF_DMX_TSO_PORT_ATTR_S *PortInfo = &g_pDmxDevOsi->TSOPortInfo[PortId];

    if ((PortAttr->enClkMode >=  HI_UNF_DMX_TSO_CLK_MODE_BUTT) ||
        (PortAttr->enValidMode >=  HI_UNF_DMX_TSO_VALID_ACTIVE_BUTT ) ||
        (PortAttr->enClk >=  HI_UNF_DMX_TSO_CLK_BUTT ) )
    {
        HI_ERR_DEMUX("enClkMode or enValidMode or enClk is invalid\n");
        return HI_ERR_DMX_INVALID_PARA;
    }

    if ( (PortAttr->u32ClkDiv < 2) || (PortAttr->u32ClkDiv > 32) || (PortAttr->u32ClkDiv%2 != 0 ) )
    {
        HI_ERR_DEMUX("u32ClkDiv = %d is invalid\n",PortAttr->u32ClkDiv );
        return HI_ERR_DMX_INVALID_PARA;
    }

    if ( (PortAttr->enClkMode == HI_UNF_DMX_TSO_CLK_MODE_NORMAL) && (PortAttr->enValidMode == HI_UNF_DMX_TSO_VALID_ACTIVE_HIGH) )
    {
            HI_ERR_DEMUX("invalid tso port attr , can not config  (enClkMode == HI_UNF_DMX_TSO_CLK_MODE_NORMAL) \
                while (enValidMode == HI_UNF_DMX_TSO_VALID_ACTIVE_HIGH)\n");
            return HI_ERR_DMX_INVALID_PARA;
    }

    if ( (PortAttr->enTSSource >= HI_UNF_DMX_PORT_0_TUNER) && (PortAttr->enTSSource <= HI_UNF_DMX_PORT_4_TUNER)  )
    {
        if ( PortAttr->enTSSource - HI_UNF_DMX_PORT_0_TUNER >= DMX_DVBPORT_CNT )
        {
            HI_ERR_DEMUX("if enTSSource is HI_UNF_DMX_PORT_TS_x , must less than 0x%x\n",HI_UNF_DMX_PORT_0_TUNER + DMX_DVBPORT_CNT);
            return HI_ERR_DMX_INVALID_PARA;
        }
    }
    else if ( (PortAttr->enTSSource >= HI_UNF_DMX_PORT_3_RAM) && (PortAttr->enTSSource <= HI_UNF_DMX_PORT_6_RAM) )
    {
        if ( PortAttr->enTSSource - HI_UNF_DMX_PORT_3_RAM >= DMX_RAMPORT_CNT )
        {
            HI_ERR_DEMUX("if enTSSource is HI_UNF_DMX_PORT_RAM_x , must less than 0x%x\n",HI_UNF_DMX_PORT_3_RAM + DMX_RAMPORT_CNT);
            return HI_ERR_DMX_INVALID_PARA;
        }
    }
    else
    {
        HI_ERR_DEMUX("enTSSource is invalid\n");
        return HI_ERR_DMX_INVALID_PARA;
    }

    PortInfo->bEnable         = PortAttr->bEnable;
    PortInfo->bClkReverse     = PortAttr->bClkReverse;
    PortInfo->enTSSource      = PortAttr->enTSSource;
    PortInfo->enClkMode       = PortAttr->enClkMode;
    PortInfo->enValidMode     = PortAttr->enValidMode;
    PortInfo->bBitSync        = PortAttr->bBitSync;
    PortInfo->bSerial         = PortAttr->bSerial;
    PortInfo->bLSB            = PortAttr->bLSB;
    PortInfo->enClk           = PortAttr->enClk;
    PortInfo->u32ClkDiv       = PortAttr->u32ClkDiv;

    DmxHalTSOPortSetAttr(PortId,PortInfo);
    DmxHalCfgTSOClk(PortId,PortInfo->bClkReverse,(HI_U32)PortInfo->enClk,PortInfo->u32ClkDiv);

    if ( PortInfo->enTSSource >=  HI_UNF_DMX_PORT_3_RAM)
    {
        DMXConfigIPPortRate(PortInfo->enTSSource - HI_UNF_DMX_PORT_3_RAM);
    }

    return HI_SUCCESS;
}
#endif
    
HI_S32 DMX_OsiAttachPort(const HI_U32 DmxId, const DMX_PORT_MODE_E PortMode, const HI_U32 PortId)
{
    DMX_Sub_DevInfo_S  *DmxInfo = &g_pDmxDevOsi->SubDevInfo[DmxId];

    DmxInfo->PortMode   = PortMode;
    DmxInfo->PortId     = PortId;

    DmxHalSetDemuxPortId(DmxId, PortMode, PortId);

    if (DMX_PORT_MODE_RAM == PortMode)
    {
        HI_U32 i;

        for (i = 0;  i < DMX_CHANNEL_CNT; i++)
        {
            DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[i];

            if (ChanInfo->DmxId == DmxId)
            {
                if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY != (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
                {
                    continue;
                }

                if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
                {
                    DMX_OQ_Info_S *OqInfo = &g_pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

                    DmxHalAttachIPBPFQ(DmxInfo->PortId, OqInfo->u32FQId);
                }
            }
        }
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiDetachPort(const HI_U32 DmxId)
{
    DMX_Sub_DevInfo_S *DmxInfo = &g_pDmxDevOsi->SubDevInfo[DmxId];

    if ((DMX_PORT_MODE_RAM == DmxInfo->PortMode) && (DmxInfo->PortId < DMX_RAMPORT_CNT))
    {
        HI_U32 i;

        for (i = 0; i < DMX_CHANNEL_CNT; i++)
        {
            DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[i];

            if (ChanInfo->DmxId == DmxId)
            {
                if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY != (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
                {
                    continue;
                }

                if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
                {
                    DMX_OQ_Info_S *OqInfo = &g_pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

                    DmxHalDetachIPBPFQ(DmxInfo->PortId, OqInfo->u32FQId);
                    DmxHalClrIPFqBPStatus(DmxInfo->PortId, OqInfo->u32FQId);
                }
            }
        }
    }

    DmxInfo->PortMode   = DMX_PORT_MODE_BUTT;
    DmxInfo->PortId     = DMX_INVALID_PORT_ID;

    DmxHalSetDemuxPortId(DmxId, DMX_PORT_MODE_BUTT, DMX_INVALID_PORT_ID);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiGetPortId(const HI_U32 DmxId, DMX_PORT_MODE_E *PortMode, HI_U32 *PortId)
{
    HI_S32              ret     = HI_ERR_DMX_NOATTACH_PORT;
    DMX_Sub_DevInfo_S  *DmxInfo = &g_pDmxDevOsi->SubDevInfo[DmxId];

    if (DMX_INVALID_PORT_ID != DmxInfo->PortId)
    {
        *PortMode   = DmxInfo->PortMode;
        *PortId     = DmxInfo->PortId;

        ret = HI_SUCCESS;
    }
    else
    {
        HI_WARN_DEMUX("the demux not attach with any port, DmxId=%d.\n", DmxId);
    }
    
    return ret;
}

HI_S32 DMX_OsiTunerPortGetAttr(const HI_U32 PortId, HI_UNF_DMX_PORT_ATTR_S *PortAttr)
{
    DMX_TunerPort_Info_S *PortInfo = &g_pDmxDevOsi->TunerPortInfo[PortId];

    PortAttr->enPortMod             = HI_UNF_DMX_PORT_MODE_EXTERNAL;
    PortAttr->enPortType            = PortInfo->PortType;
    PortAttr->u32SyncLostTh         = PortInfo->SyncLostTh;
    PortAttr->u32SyncLockTh         = PortInfo->SyncLockTh;
    PortAttr->u32TunerInClk         = PortInfo->TunerInClk;
    PortAttr->u32SerialBitSelector  = PortInfo->BitSelector;
    PortAttr->u32TunerErrMod        = 0;
    PortAttr->u32UserDefLen1        = 0;
    PortAttr->u32UserDefLen2        = 0;

    return HI_SUCCESS;
}

HI_S32 DMX_OsiTunerPortSetAttr(const HI_U32 PortId, const HI_UNF_DMX_PORT_ATTR_S *PortAttr)
{
    DMX_TunerPort_Info_S *PortInfo = &g_pDmxDevOsi->TunerPortInfo[PortId];

    switch (PortAttr->enPortType)
    {
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_BURST :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_VALID :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188 :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_204 :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188_204 :
        case HI_UNF_DMX_PORT_TYPE_SERIAL :
            break;

        case HI_UNF_DMX_PORT_TYPE_SERIAL2BIT :
        case HI_UNF_DMX_PORT_TYPE_SERIAL_NOSYNC :
        case HI_UNF_DMX_PORT_TYPE_SERIAL2BIT_NOSYNC :
    #ifdef DMX_TUNER_PORT_SERIAL_2BIT_AND_SERIAL_NOSYNC_SUPPORT
            break;
    #else
            return HI_ERR_DMX_NOT_SUPPORT;
    #endif

        case HI_UNF_DMX_PORT_TYPE_USER_DEFINED :
        case HI_UNF_DMX_PORT_TYPE_AUTO :
            return HI_ERR_DMX_NOT_SUPPORT;

        default :
            HI_ERR_DEMUX("Port %u set invalid port type %d\n", PortId, PortAttr->enPortType);

            return HI_ERR_DMX_INVALID_PARA;
    }

    if (PortAttr->u32TunerInClk > 1)
    {
        HI_ERR_DEMUX("Port %u set invalid tunner in clock %d\n", PortId, PortAttr->u32TunerInClk);

        return HI_ERR_DMX_INVALID_PARA;
    }

    if (PortAttr->u32SerialBitSelector > 1)
    {
        HI_ERR_DEMUX("Port %u set invalid Serial Bit Selector %d\n", PortId, PortAttr->u32SerialBitSelector);

        return HI_ERR_DMX_INVALID_PARA;
    }

    PortInfo->PortType      = PortAttr->enPortType;
    PortInfo->SyncLockTh    = PortAttr->u32SyncLockTh > DMX_MAX_LOCK_TH ? DMX_MAX_LOCK_TH : PortAttr->u32SyncLockTh;
    PortInfo->SyncLostTh    = PortAttr->u32SyncLostTh > DMX_MAX_LOST_TH ? DMX_MAX_LOST_TH : PortAttr->u32SyncLostTh;
    PortInfo->BitSelector   = PortAttr->u32SerialBitSelector;

    DmxHalDvbPortSetAttr(PortId, PortInfo->PortType, PortInfo->SyncLockTh,
            PortInfo->SyncLostTh, PortInfo->TunerInClk, PortInfo->BitSelector);

    if (0 != PortId)
    {
        PortInfo->TunerInClk = PortAttr->u32TunerInClk;

        DmxHalDvbPortSetClkInPol(PortId, PortInfo->TunerInClk);
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiRamPortGetAttr(const HI_U32 PortId, HI_UNF_DMX_PORT_ATTR_S *PortAttr)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];

    PortAttr->enPortMod             = HI_UNF_DMX_PORT_MODE_RAM;
    PortAttr->enPortType            = PortInfo->PortType;
    PortAttr->u32SyncLostTh         = PortInfo->SyncLostTh;
    PortAttr->u32SyncLockTh         = PortInfo->SyncLockTh;
    PortAttr->u32TunerInClk         = 0;
    PortAttr->u32SerialBitSelector  = 0;
    PortAttr->u32TunerErrMod        = 0;
    PortAttr->u32UserDefLen1        = PortInfo->MinLen;
    PortAttr->u32UserDefLen2        = PortInfo->MaxLen;

    return HI_SUCCESS;
}

HI_S32 DMX_OsiRamPortSetAttr(const HI_U32 PortId, const HI_UNF_DMX_PORT_ATTR_S *PortAttr)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];

#ifdef CHIP_TYPE_hi3716m
    if (!Hi3716MV300Flag)
    {
        if (HI_UNF_DMX_PORT_TYPE_USER_DEFINED == PortAttr->enPortType)
        {
            return HI_ERR_DMX_NOT_SUPPORT;
        }
    }
#endif

    switch (PortAttr->enPortType)
    {
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188 :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_204 :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_NOSYNC_188_204 :
            PortInfo->MinLen = 0;
            PortInfo->MaxLen = 0;

            break;

        case HI_UNF_DMX_PORT_TYPE_AUTO :
    #ifdef DMX_RAM_PORT_AUTO_SCAN_SUPPORT
            PortInfo->MinLen = 0;
            PortInfo->MaxLen = 0;

            break;
    #else
            HI_ERR_DEMUX("do not support auto scan!\n");
            return HI_ERR_DMX_NOT_SUPPORT;
    #endif

        case HI_UNF_DMX_PORT_TYPE_USER_DEFINED :
    #ifdef DMX_RAM_PORT_SET_LENGTH_SUPPORT
            if (   (PortAttr->u32UserDefLen1 < DMX_RAM_PORT_MIN_LEN)
                || (PortAttr->u32UserDefLen1 > DMX_RAM_PORT_MAX_LEN)
                || (PortAttr->u32UserDefLen2 < DMX_RAM_PORT_MIN_LEN)
                || (PortAttr->u32UserDefLen2 > DMX_RAM_PORT_MAX_LEN) )
            {
                HI_ERR_DEMUX("set len error. len1=%u, len2=%u\n", PortAttr->u32UserDefLen1, PortAttr->u32UserDefLen2);

                return HI_ERR_DMX_INVALID_PARA;
            }

            PortInfo->MinLen = PortAttr->u32UserDefLen1;
            PortInfo->MaxLen = PortAttr->u32UserDefLen2;

            break;
    #else
            return HI_ERR_DMX_NOT_SUPPORT;
    #endif

        case HI_UNF_DMX_PORT_TYPE_PARALLEL_BURST :
        case HI_UNF_DMX_PORT_TYPE_PARALLEL_VALID :
        case HI_UNF_DMX_PORT_TYPE_SERIAL :
        case HI_UNF_DMX_PORT_TYPE_SERIAL2BIT :
        case HI_UNF_DMX_PORT_TYPE_SERIAL_NOSYNC :
        case HI_UNF_DMX_PORT_TYPE_SERIAL2BIT_NOSYNC :
            return HI_ERR_DMX_NOT_SUPPORT;

        default :
            HI_ERR_DEMUX("Invalid type %u\n", PortAttr->enPortType);

            return HI_ERR_DMX_INVALID_PARA;
    }

    PortInfo->PortType      = PortAttr->enPortType;
    PortInfo->SyncLockTh    = PortAttr->u32SyncLockTh > DMX_MAX_LOCK_TH ? DMX_MAX_LOCK_TH : PortAttr->u32SyncLockTh;
    PortInfo->SyncLostTh    = PortAttr->u32SyncLostTh > DMX_MAX_LOST_TH ? DMX_MAX_LOST_TH : PortAttr->u32SyncLostTh;

    DmxHalIPPortSetAttr(PortId, PortInfo->PortType, PortInfo->SyncLockTh, PortInfo->SyncLostTh);

#ifdef CHIP_TYPE_hi3716m
    if (Hi3716MV300Flag)
    {
        if (HI_UNF_DMX_PORT_TYPE_USER_DEFINED == PortAttr->enPortType)
        {
            DmxHalIPPortSetSyncLen(PortId, PortInfo->MinLen, PortInfo->MaxLen);
        }
        else
        {
            DmxHalIPPortSetSyncLen(PortId, DMX_TS_PACKET_LEN, DMX_TS_PACKET_LEN_204);
        }
    }
#else
    #ifdef DMX_RAM_PORT_SET_LENGTH_SUPPORT
    if (HI_UNF_DMX_PORT_TYPE_USER_DEFINED == PortAttr->enPortType)
    {
        DmxHalIPPortSetSyncLen(PortId, PortInfo->MinLen, PortInfo->MaxLen);
    }
    else
    {
        DmxHalIPPortSetSyncLen(PortId, DMX_TS_PACKET_LEN, DMX_TS_PACKET_LEN_204);
    }
    #endif
#endif

#ifdef DMX_RAM_PORT_AUTO_SCAN_SUPPORT
    if (HI_UNF_DMX_PORT_TYPE_AUTO == PortAttr->enPortType)
    {
        DmxHalIPPortSetAutoScanRegion(PortId, DMX_RAM_PORT_AUTO_REGION, DMX_RAM_PORT_AUTO_STEP_4);
    }

    /* it always add an null desc when put ts buff, so when enable it */
    DmxHalIPPortEnableJudgeNullDesc(PortId);
#endif

    return HI_SUCCESS;
}

HI_S32 DMX_OsiTunerPortGetPacketNum(const HI_U32 PortId, HI_U32 *TsPackCnt, HI_U32 *ErrTsPackCnt)
{
    *TsPackCnt      = DmxHalDvbPortGetTsPackCount(PortId);
    *ErrTsPackCnt   = DmxHalDvbPortGetErrTsPackCount(PortId);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiRamPortGetPacketNum(const HI_U32 PortId, HI_U32 *TsPackCnt)
{
    *TsPackCnt = DmxHalIPPortGetTsPackCount(PortId);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiTsBufferCreate(const HI_U32 PortId, const HI_U32 Size, DMX_MMZ_BUF_S *TsBuf)
{
    HI_S32              ret;
    DMX_RamPort_Info_S *PortInfo    = &g_pDmxDevOsi->RamPortInfo[PortId];
    HI_CHAR             BufName[16] = "DMX_TsBuf[0]";
    HI_U32              BufSize     = Size / 0x1000 * 0x1000;
    HI_U32              DescDepth;
    HI_U32              DescBufSize;
    MMZ_BUFFER_S        MmzBuf;

    if (PortInfo->PhyAddr)
    {
        HI_ERR_DEMUX("TSBuffer has been used by other process:PortId=%u\n", PortId);

        return HI_ERR_DMX_RECREAT_TSBUFFER;
    }

    if ((Size > DMX_MAX_TS_BUFFER_SIZE) || (Size < DMX_MIN_TS_BUFFER_SIZE))
    {
        HI_ERR_DEMUX("TsBuffer size 0x%x invalid, bigger than 0x%x, or less than 0x%x\n",
            Size, DMX_MAX_TS_BUFFER_SIZE, DMX_MIN_TS_BUFFER_SIZE);

        return HI_ERR_DMX_INVALID_PARA;
    }

    DescDepth = (BufSize / 0x100000) * 0x3ff;
    if (DescDepth < DMX_MIN_IP_DESC_DEPTH)
    {
        DescDepth = DMX_MIN_IP_DESC_DEPTH;
    }
    else if (DescDepth > DMX_MAX_IP_DESC_DEPTH)
    {
        DescDepth = DMX_MAX_IP_DESC_DEPTH;
    }

    DescBufSize = (DescDepth + 1) * DMX_PER_DESC_LEN;

    BufName[10] = '0' + PortId;

    ret = HI_DRV_MMZ_AllocAndMap(BufName, HI_NULL, BufSize + DescBufSize, 0, &MmzBuf);
    if (HI_SUCCESS != ret)
    {
        HI_ERR_DEMUX("malloc 0x%x failed\n", BufSize + DescBufSize);

        return HI_ERR_DMX_ALLOC_MEM_FAILED;
    }

    PortInfo->PhyAddr       = MmzBuf.u32StartPhyAddr;
    PortInfo->KerAddr       = MmzBuf.u32StartVirAddr;
    PortInfo->BufSize       = BufSize;
    PortInfo->Read          = 0;
    PortInfo->Write         = 0;

    PortInfo->ReqAddr       = 0;
    PortInfo->ReqLen        = 0;

    PortInfo->DescPhyAddr   = PortInfo->PhyAddr + BufSize;
    PortInfo->DescKerAddr   = PortInfo->KerAddr + BufSize;
    PortInfo->DescCurrAddr  = (HI_U32*)PortInfo->DescKerAddr;
    PortInfo->DescDepth     = DescDepth;
    PortInfo->DescWrite     = 0;

    spin_lock_init(&PortInfo->LockRamPort );
    
    memset((HI_VOID*)PortInfo->DescKerAddr, 0, DescBufSize);

    TsBufferConfig(PortId, HI_TRUE, PortInfo->DescPhyAddr, DescDepth);

    TsBuf->u32BufPhyAddr    = PortInfo->PhyAddr;
    TsBuf->u32BufKerVirAddr = PortInfo->KerAddr;
    TsBuf->u32BufSize       = PortInfo->BufSize;

    HI_INFO_DEMUX("port %d attach ip buffer success, buffer size=0x%x\n", PortId, PortInfo->BufSize);

    return ret;
}

HI_S32 DMX_OsiTsBufferDestroy(const HI_U32 PortId)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_RamPort_Info_S *PortInfo    = &g_pDmxDevOsi->RamPortInfo[PortId];

    if (PortInfo->PhyAddr)
    {
        MMZ_BUFFER_S MmzBuf;

        TsBufferConfig(PortId, HI_FALSE, 0, 0);

        MmzBuf.u32StartPhyAddr = PortInfo->PhyAddr;
        MmzBuf.u32StartVirAddr = PortInfo->KerAddr;

        HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);

        PortInfo->PhyAddr       = 0;
        PortInfo->DescPhyAddr   = 0;

        RamPortClean(PortId);

        ret = HI_SUCCESS;
    }
    else
    {
        HI_ERR_DEMUX("invalid PhyAddr!\n");
    }

    return ret;
}

HI_S32 DMX_OsiTsBufferGet(const HI_U32 PortId, const HI_U32 ReqLen, DMX_DATA_BUF_S *Buf, const HI_U32 Timeout)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];

    if (0 == PortInfo->PhyAddr)
    {
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (0 == ReqLen)
    {
        HI_ERR_DEMUX("port %d get buf len is 0\n", PortId);

        return HI_ERR_DMX_INVALID_PARA;
    }

    if (ReqLen > PortInfo->BufSize)
    {
        HI_ERR_DEMUX("error, ReqLen(0x%x) > BufSize(0x%x)\n", ReqLen, PortInfo->BufSize);

        return HI_ERR_DMX_INVALID_PARA;
    }

    ++PortInfo->GetCount;

    if (CheckIPBuf(PortId, ReqLen))
    {
        if (0 == Timeout)
        {
            return HI_ERR_DMX_NOAVAILABLE_BUF;
        }

        PortInfo->WakeUp    = HI_FALSE;
        PortInfo->WaitLen   = ReqLen;

        wait_event_interruptible_timeout(PortInfo->WaitQueue, PortInfo->WakeUp, msecs_to_jiffies(Timeout));
        if (!PortInfo->WakeUp || (0 == PortInfo->ReqAddr))
        {
            HI_WARN_DEMUX("timeout\n");

            return HI_ERR_DMX_TIMEOUT;
        }
    }

    Buf->BufPhyAddr = PortInfo->ReqAddr;
    Buf->BufKerAddr = PortInfo->KerAddr + (PortInfo->ReqAddr - PortInfo->PhyAddr);
    Buf->BufLen     = ReqLen;

    ++PortInfo->GetValidCount;

    return HI_SUCCESS;
}

extern HI_VOID DMX_OsrSaveIPTs(HI_U8 *buf, HI_U32 len,HI_U32 u32PortID);

HI_S32 DMX_OsiTsBufferPut(const HI_U32 PortId, const HI_U32 DataLen, const HI_U32 StartPos)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];
    HI_U32              BufferAddr;
    HI_U32              BufferLen;
    HI_U32              Offset;
    HI_SIZE_T           LockFlag;

    if (0 == PortInfo->PhyAddr)
    {
        HI_ERR_DEMUX("put phyaddr is 0!");
        return HI_ERR_DMX_INVALID_PARA;
    }

    if ((0 == PortInfo->ReqLen) || (0 == DataLen))
    {
        return HI_SUCCESS;
    }

    if (PortInfo->ReqLen < StartPos + DataLen)
    {
        HI_ERR_DEMUX("port %u put buf len %u, request len %u\n", PortId, DataLen, PortInfo->ReqLen);

        return HI_ERR_DMX_INVALID_PARA;
    }

    spin_lock_irqsave(&PortInfo->LockRamPort, LockFlag);
    Offset = PortInfo->ReqAddr - PortInfo->PhyAddr;

    PortInfo->Write = Offset + DataLen + StartPos;

    BufferAddr  = PortInfo->ReqAddr + StartPos;
    BufferLen   = DataLen;

    PortInfo->ReqLen    = 0;
    PortInfo->ReqAddr   = 0;
    spin_unlock_irqrestore(&PortInfo->LockRamPort, LockFlag);

#ifdef HI_DEMUX_PROC_SUPPORT
    DMX_OsrSaveIPTs((HI_U8*)(PortInfo->KerAddr + Offset + StartPos), DataLen, PortId);

    ++PortInfo->PutCount;
#endif

    do
    {
        HI_U32 len = (BufferLen >= DMX_MAX_IP_BLOCK_SIZE) ? DMX_MAX_IP_BLOCK_SIZE : BufferLen;

        // set ip descriptor
        *PortInfo->DescCurrAddr++ = BufferAddr;
        *PortInfo->DescCurrAddr++ = len;
        *PortInfo->DescCurrAddr++ = len;
        *PortInfo->DescCurrAddr++ = 0;

        if (++PortInfo->DescWrite >= PortInfo->DescDepth)
        {
            PortInfo->DescWrite     = 0;
            PortInfo->DescCurrAddr  = (HI_U32*)PortInfo->DescKerAddr;
        }

        DmxHalIPPortDescAdd(PortId, 1);

        if (0 == len)
        {
            break;
        }

        BufferAddr  += len;
        BufferLen   -= len;
    } while (1);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiTsBufferReset(const HI_U32 PortId)
{
    DMX_RamPort_Info_S *PortInfo = &g_pDmxDevOsi->RamPortInfo[PortId];

    if (PortInfo->PhyAddr)
    {
        RamPortClean(PortId);
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiTsBufferGetStatus(const HI_U32 PortId, HI_UNF_DMX_TSBUF_STATUS_S *Status)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_RamPort_Info_S *PortInfo    = &g_pDmxDevOsi->RamPortInfo[PortId];
    HI_U32              Head;
    HI_U32              Tail;
    HI_SIZE_T           LockFlag;

    spin_lock_irqsave(&PortInfo->LockRamPort, LockFlag);
    if (PortInfo->PhyAddr)
    {
        Status->u32BufSize  = PortInfo->BufSize;
        Status->u32UsedSize = PortInfo->BufSize;

        if (PortInfo->Read < PortInfo->Write)
        {
            Head = PortInfo->Read;
            Tail = PortInfo->BufSize - PortInfo->Write;

            Head = (Head <= DMX_TS_BUFFER_GAP) ? 0 : (Head - DMX_TS_BUFFER_GAP);
            Tail = (Tail <= DMX_TS_BUFFER_GAP) ? 0 : (Tail - DMX_TS_BUFFER_GAP);

            if (Head < Tail)
            {
                Status->u32UsedSize = PortInfo->BufSize - Tail;
            }
            else
            {
                Status->u32UsedSize = PortInfo->BufSize - Head;
            }
        }
        else if (PortInfo->Read > PortInfo->Write)
        {
            Head = PortInfo->Read - PortInfo->Write;
            if (Head > DMX_TS_BUFFER_GAP)
            {
                Status->u32UsedSize = DMX_TS_BUFFER_GAP + PortInfo->BufSize - Head;
            }
        }
        else
        {
            if (0 == PortInfo->Read)
            {
                Status->u32UsedSize = DMX_TS_BUFFER_GAP;
            }
        }

        ret = HI_SUCCESS;
    }
    spin_unlock_irqrestore(&PortInfo->LockRamPort, LockFlag);

    return ret;
}

/***********************************************************************************
* Function      : DMX_OsiNewFilter
* Description   : apply a new filter
* Input         : DmxId
* Output        : FilterId
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiNewFilter(const HI_U32 DmxId, HI_U32 *FilterId)
{
    HI_S32              ret         = HI_ERR_DMX_NOFREE_FILTER;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
#ifdef DMX_REGION_SUPPORT
    DMX_Sub_DevInfo_S  *DmxInfo     = &DmxDevOsi->SubDevInfo[DmxId];
#endif
    DMX_FilterInfo_S   *FilterInfo  = DmxDevOsi->DmxFilterInfo;
    HI_U32              RegionBegin = 0;
    HI_U32              RegionEnd   = DMX_FILTER_CNT;
    HI_U32              i;

#ifdef DMX_REGION_SUPPORT
    if (DmxInfo->DmxFilterCount >= DMX_REGION_FILTER_COUNT)
    {
        return HI_ERR_DMX_NOFREE_FILTER;
    }

    RegionBegin = DMX_GET_REGION_ID(DmxId) * DMX_REGION_FILTER_COUNT;
    RegionEnd   = RegionBegin + DMX_REGION_FILTER_COUNT;
#endif

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        for (i = RegionBegin; i < RegionEnd; i++)
        {
            if (DMX_INVALID_FILTER_ID == FilterInfo[i].FilterId)
            {
                FilterInfo[i].FilterId  = i;
                FilterInfo[i].ChanId    = DMX_INVALID_CHAN_ID;
                FilterInfo[i].Depth     = 0;

                *FilterId = i;

            #ifdef DMX_REGION_SUPPORT
                FilterInfo[i].DmxId = DmxId;

                ++DmxInfo->DmxFilterCount;
            #endif

                ret = HI_SUCCESS;

                break;
            }
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

/***********************************************************************************
* Function      :  DMX_OsiDeleteFilter
* Description   :  delete a filter
* Input         :  FilterId
* Output        :
* Return        :  HI_SUCCESS:     success
*                  HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiDeleteFilter(const HI_U32 FilterId)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];
    DMX_ChanInfo_S     *Chan        = HI_NULL;

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        if (DMX_INVALID_FILTER_ID != Filter->FilterId)
        {
        #ifdef DMX_REGION_SUPPORT
            DMX_Sub_DevInfo_S  *DmxInfo = &DmxDevOsi->SubDevInfo[Filter->DmxId];

            --DmxInfo->DmxFilterCount;
        #endif

            if (DMX_INVALID_CHAN_ID != Filter->ChanId)
            {
                Chan = &DmxDevOsi->DmxChanInfo[Filter->ChanId];

                DmxHalDetachFilter(FilterId, Filter->ChanId);
            }

            Filter->FilterId    = DMX_INVALID_FILTER_ID;
            Filter->ChanId      = DMX_INVALID_CHAN_ID;

            ret = HI_SUCCESS;
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);

        if (Chan)
        {
            if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Channel))
            {
                --Chan->FilterCount;

                mutex_unlock(&DmxDevOsi->lock_Channel);
            }
            else
            {
                HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
                ret = HI_ERR_DMX_BUSY;
            }
        }
    }
    else
    {
        ret = HI_ERR_DMX_BUSY;
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
    }

    return ret;
}

/***********************************************************************************
* Function      :  DMX_OsiSetFilterAttr
* Description   :  set filter attr
* Input         :  FilterId, FilterAttr
* Output        :
* Return        :  HI_SUCCESS:     success
*                  HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiSetFilterAttr(const HI_U32 FilterId, const HI_UNF_DMX_FILTER_ATTR_S *FilterAttr)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];
    HI_U32              i;

    if (FilterAttr->u32FilterDepth > DMX_FILTER_MAX_DEPTH)
    {
        HI_ERR_DEMUX("filter %u set depth is %u too long\n", FilterId, FilterAttr->u32FilterDepth);

        return HI_ERR_DMX_INVALID_PARA;
    }

    for (i = 0; i < FilterAttr->u32FilterDepth; i++)
    {
        if (FilterAttr->au8Negate[i] > 1)
        {
            HI_ERR_DEMUX("filter %u set negate is invalid\n", FilterId);

            return HI_ERR_DMX_INVALID_PARA;
        }
    }

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        if (DMX_INVALID_FILTER_ID != Filter->FilterId)
        {
            Filter->Depth = FilterGetValidDepth(FilterAttr);

            for (i = 0; i < DMX_FILTER_MAX_DEPTH; i++)
            {
                if (i < FilterAttr->u32FilterDepth)
                {
                    HI_U32 Negate = FilterAttr->au8Negate[i];

                    if (0xff == FilterAttr->au8Mask[i])
                    {
                        Negate = 0;
                    }

                    Filter->Match[i]   = FilterAttr->au8Match[i];
                    Filter->Mask[i]    = FilterAttr->au8Mask[i];
                    Filter->Negate[i]  = Negate;

                    DmxHalSetFilter(FilterId, i, Filter->Match[i], Negate, Filter->Mask[i]);
                }
                else
                {
                    Filter->Match[i]   = 0;
                    Filter->Mask[i]    = 0xff;
                    Filter->Negate[i]  = 0;

                    DmxHalSetFilter(FilterId, i, 0, 0, 0xff);
                }
            }

        #ifdef DMX_FILTER_DEPTH_SUPPORT
            DmxHalFilterSetDepth(FilterId, Filter->Depth);
        #endif

            ret = HI_SUCCESS;
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);
    }
    else
    {
        ret = HI_ERR_DMX_BUSY;
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
    }

    return ret;
}

/***********************************************************************************
* Function      :  DMX_OsiGetFilterAttr
* Description   :  get filter attr
* Input         :  FilterId
* Output        :  FilterAttr
* Return        :  HI_SUCCESS:     success
*                  HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiGetFilterAttr(const HI_U32 FilterId, HI_UNF_DMX_FILTER_ATTR_S *FilterAttr)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        if (DMX_INVALID_FILTER_ID != Filter->FilterId)
        {
            FilterAttr->u32FilterDepth = Filter->Depth;

            memcpy(FilterAttr->au8Match,    Filter->Match,  DMX_FILTER_MAX_DEPTH);
            memcpy(FilterAttr->au8Mask,     Filter->Mask,   DMX_FILTER_MAX_DEPTH);
            memcpy(FilterAttr->au8Negate,   Filter->Negate, DMX_FILTER_MAX_DEPTH);

            ret = HI_SUCCESS;
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);
    }
    else
    {
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

/***********************************************************************************
* Function      :  DMX_OsiAttachFilter
* Description   :  attach filter to channel
* Input         :  FilterId ChanId
* Output        :
* Return        :  HI_SUCCESS:     success
*                  HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiAttachFilter(const HI_U32 FilterId, const HI_U32 ChanId)
{
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_ChanInfo_S     *Chan        = &DmxDevOsi->DmxChanInfo[ChanId];
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];

    if ((DMX_INVALID_CHAN_ID == Chan->ChanId) || (DMX_INVALID_FILTER_ID == Filter->FilterId))
    {
        HI_ERR_DEMUX("ChanId:%d or FilterId:%d invalid!\n",Chan->ChanId,Filter->FilterId);
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (DMX_INVALID_CHAN_ID != Filter->ChanId)
    {
        HI_ERR_DEMUX("filter already attached!\n");
        return HI_ERR_DMX_ATTACHED_FILTER;
    }

    if (   (HI_UNF_DMX_CHAN_TYPE_SEC    != Chan->ChanType)
        && (HI_UNF_DMX_CHAN_TYPE_ECM_EMM!= Chan->ChanType)
        && (HI_UNF_DMX_CHAN_TYPE_PES    != Chan->ChanType) )
    {
        HI_ERR_DEMUX("invalid channel type\n");

        return HI_ERR_DMX_NOT_SUPPORT;
    }

    if (Chan->FilterCount >= DMX_MAX_FILTER_NUM_PER_CHANNEL)
    {
        HI_ERR_DEMUX("channel %u has maximum filters\n", ChanId);

        return HI_ERR_DMX_NOT_SUPPORT;
    }

#ifdef DMX_REGION_SUPPORT
    if (DMX_GET_REGION_ID(Chan->DmxId) != DMX_GET_REGION_ID(Filter->DmxId))
    {
        HI_ERR_DEMUX("filter(%u) and chan(%u) must in the same region\n", FilterId, ChanId);

        return HI_ERR_DMX_INVALID_PARA;
    }
#endif

    if (0 != mutex_lock_interruptible(&DmxDevOsi->lock_Channel))
    {
        HI_ERR_DEMUX("down_interruptible failed!\n");
        return HI_ERR_DMX_BUSY;
    }

    ++Chan->FilterCount;

    mutex_unlock(&DmxDevOsi->lock_Channel);

    if (0 != mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        return HI_ERR_DMX_BUSY;
    }

    Filter->ChanId = ChanId;

    mutex_unlock(&DmxDevOsi->lock_Filter);

    DmxHalAttachFilter(FilterId, ChanId);

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiDetachFilter
* Description   : detach filter from a channel
* Input         : FilterId, ChanId
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiDetachFilter(const HI_U32 FilterId, const HI_U32 ChanId)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_ChanInfo_S     *Chan        = &DmxDevOsi->DmxChanInfo[ChanId];
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        if (DMX_INVALID_FILTER_ID != Filter->FilterId)
        {
            if (DMX_INVALID_CHAN_ID != Filter->ChanId)
            {
                if (ChanId == Filter->ChanId)
                {
                    DmxHalDetachFilter(FilterId, ChanId);

                    Filter->ChanId = DMX_INVALID_CHAN_ID;

                    ret = HI_SUCCESS;
                }
                else
                {
                    ret = HI_ERR_DMX_UNMATCH_FILTER;
                }
            }
            else
            {
                ret = HI_ERR_DMX_NOATTACH_FILTER;
            }
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);

        if (HI_SUCCESS == ret)
        {
            if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Channel))
            {
                --Chan->FilterCount;

                mutex_unlock(&DmxDevOsi->lock_Channel);
            }
            else
            {
                ret = HI_ERR_DMX_BUSY;
            }
        }
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

/***********************************************************************************
* Function      :   DMX_OsiGetFilterChannel
* Description   :  Get  filter attached channel
* Input         : FilterId
* Output        : pChanId
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiGetFilterChannel(const HI_U32 FilterId, HI_U32 *ChanId)
{
    HI_S32              ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_FilterInfo_S   *Filter      = &DmxDevOsi->DmxFilterInfo[FilterId];

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Filter))
    {
        if (DMX_INVALID_FILTER_ID != Filter->FilterId)
        {
            if (DMX_INVALID_CHAN_ID != Filter->ChanId)
            {
                *ChanId = Filter->ChanId;

                ret = HI_SUCCESS;
            }
            else
            {
                ret = HI_ERR_DMX_NOATTACH_FILTER;
            }
        }

        mutex_unlock(&DmxDevOsi->lock_Filter);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

/***********************************************************************************
* Function      : DMX_OsiGetFreeFilterNum
* Description   : Get free filter count
* Input         : DmxId
* Output        : FreeCount
* Return        : HI_SUCCESS
*                 HI_FAILURE
* Others:
***********************************************************************************/
HI_S32 DMX_OsiGetFreeFilterNum(const HI_U32 DmxId, HI_U32 *FreeCount)
{
    DMX_FilterInfo_S   *FilterInfo  = g_pDmxDevOsi->DmxFilterInfo;
    HI_U32              RegionBegin = 0;
    HI_U32              RegionEnd   = DMX_FILTER_CNT;
    HI_U32              i;

    *FreeCount = 0;

#ifdef DMX_REGION_SUPPORT
    RegionBegin = DMX_GET_REGION_ID(DmxId) * DMX_REGION_FILTER_COUNT;
    RegionEnd   = RegionBegin + DMX_REGION_FILTER_COUNT;
#endif

    for (i = RegionBegin; i < RegionEnd; i++)
    {
        if (DMX_INVALID_FILTER_ID == FilterInfo[i].FilterId)
        {
            ++(*FreeCount);
        }
    }

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiCreateChannel
* Description   : create a channel
* Input         : DmxId
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiCreateChannel(HI_U32 DmxId, HI_UNF_DMX_CHAN_ATTR_S *ChanAttr, DMX_MMZ_BUF_S *ChanBuf, HI_U32 *ChanId)
{
    HI_S32                      ret;
    DMX_DEV_OSI_S              *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S             *ChanInfo    = HI_NULL;
    DMX_OQ_Info_S              *OqInfo      = HI_NULL;
    HI_UNF_DMX_CHAN_CRC_MODE_E  CrcMode     = HI_UNF_DMX_CHAN_CRC_MODE_FORBID;
    HI_U32                      RegionBegin = 0;
    HI_U32                      RegionEnd   = DMX_CHANNEL_CNT;
    HI_U32                      BufSize;
    HI_U32                      i;

    BufSize = ChanAttr->u32BufSize;
    if (BufSize < MIN_MMZ_BUF_SIZE)
    {
        BufSize = MIN_MMZ_BUF_SIZE;
    }

    switch (ChanAttr->enChannelType)
    {
        case HI_UNF_DMX_CHAN_TYPE_SEC :
        case HI_UNF_DMX_CHAN_TYPE_ECM_EMM :
            switch (ChanAttr->enCRCMode)
            {
                case HI_UNF_DMX_CHAN_CRC_MODE_FORBID :
                case HI_UNF_DMX_CHAN_CRC_MODE_FORCE_AND_DISCARD :
                case HI_UNF_DMX_CHAN_CRC_MODE_FORCE_AND_SEND :
                case HI_UNF_DMX_CHAN_CRC_MODE_BY_SYNTAX_AND_DISCARD :
                case HI_UNF_DMX_CHAN_CRC_MODE_BY_SYNTAX_AND_SEND :
                    CrcMode = ChanAttr->enCRCMode;

                    break;

                default :
                    HI_ERR_DEMUX("invalid crc mode %u\n", ChanAttr->enCRCMode);

                    return HI_ERR_DMX_INVALID_PARA;
            }

            break;

        case HI_UNF_DMX_CHAN_TYPE_PES :
        case HI_UNF_DMX_CHAN_TYPE_AUD :
        case HI_UNF_DMX_CHAN_TYPE_VID :
            if (BufSize < DMX_PES_CHANNEL_MIN_SIZE)
            {
                BufSize = DMX_PES_CHANNEL_MIN_SIZE;
            }

            break;

        case HI_UNF_DMX_CHAN_TYPE_POST :
            break;

        default :
            HI_ERR_DEMUX("invalid chan type %u\n", ChanAttr->enChannelType);

            return HI_ERR_DMX_INVALID_PARA;
    }

    switch (ChanAttr->enOutputMode)
    {
        case HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY :
        case HI_UNF_DMX_CHAN_OUTPUT_MODE_REC :
        case HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY_REC :
            break;

        default :
            HI_ERR_DEMUX("Invalid output mode %u\n", ChanAttr->enOutputMode);

            return HI_ERR_DMX_INVALID_PARA;
    }

#ifdef DMX_REGION_SUPPORT
    RegionBegin = DMX_GET_REGION_ID(DmxId) * DMX_REGION_CHANNEL_COUNT;
    RegionEnd   = RegionBegin + DMX_REGION_CHANNEL_COUNT;
#endif

    if (0 == mutex_lock_interruptible(&DmxMgr->lock_Channel))
    {
        for (i = RegionBegin; i < RegionEnd; i++)
        {
            if (DmxMgr->DmxChanInfo[i].DmxId >= DMX_CNT)
            {
                DmxMgr->DmxChanInfo[i].DmxId = DmxId;

                break;
            }
        }

        mutex_unlock(&DmxMgr->lock_Channel);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        return HI_ERR_DMX_BUSY;
    }

    if (i >= RegionEnd)
    {
        HI_ERR_DEMUX("no free channel\n");

        return HI_ERR_DMX_NOFREE_CHAN;
    }

    ChanInfo = &DmxMgr->DmxChanInfo[i];

    // aligned by 4K bytes
    BufSize = (BufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;

    ChanInfo->ChanId        = i;
    ChanInfo->ChanPid       = DMX_INVALID_PID;
    ChanInfo->FilterCount   = 0;
    ChanInfo->ChanBufSize   = BufSize;
    ChanInfo->ChanType      = ChanAttr->enChannelType;
    ChanInfo->ChanCrcMode   = CrcMode;
    ChanInfo->ChanOutMode   = ChanAttr->enOutputMode;
    ChanInfo->ChanStatus    = HI_UNF_DMX_CHAN_CLOSE;
    ChanInfo->KeyId         = DMX_INVALID_KEY_ID;
    ChanInfo->ChanOqId      = i;
    ChanInfo->u32TotolAcq   = 0;
    ChanInfo->u32HitAcq     = 0;
    ChanInfo->u32Release    = 0;
    ChanInfo->LastPts       = INVALID_PTS;
    ChanInfo->ChanEosFlag   = HI_FALSE;
    memset((HI_U8 *)&ChanInfo->stLastControl,0,sizeof(Disp_Control_t));

#ifdef DMX_USE_ECM
    ChanInfo->u32SwFlag     = 0;
#endif

    OqInfo = &DmxMgr->DmxOqInfo[ChanInfo->ChanOqId];

    memset(OqInfo, 0, sizeof(DMX_OQ_Info_S));

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
        DMX_FQ_Info_S  *FqInfo  = HI_NULL;
        HI_U32          FqId    = DMX_FQ_COMMOM;
        HI_U32          OqDepth;
        HI_U32          OqBufSize;
        HI_CHAR         BufName[16];
        MMZ_BUFFER_S    MmzBuf;

        if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
        {
            HI_U32      FqDepth;
            HI_U32      FqBufSize;
            HI_CHAR    *str;
            HI_U32      BlockSize;

            mutex_lock(&DmxMgr->lock_AVChan);
            if (DmxMgr->AVChanCount >= DMX_AV_CHANNEL_CNT)
            {
                mutex_unlock(&DmxMgr->lock_AVChan);

                HI_ERR_DEMUX("no free av channel\n");

                ChanInfo->DmxId = DMX_INVALID_DEMUX_ID;

                return HI_ERR_DMX_NOFREE_CHAN;
            }

            ++DmxMgr->AVChanCount;
            mutex_unlock(&DmxMgr->lock_AVChan);

            FqId = DmxFqIdAcquire(1, DMX_FQ_CNT);
            if (FqId >= DMX_FQ_CNT)
            {
                HI_ERR_DEMUX("no free fq\n");

                ChanInfo->DmxId = DMX_INVALID_DEMUX_ID;

                mutex_lock(&DmxMgr->lock_AVChan);
                --DmxMgr->AVChanCount;
                mutex_unlock(&DmxMgr->lock_AVChan);

                return HI_ERR_DMX_NOFREE_CHAN;
            }

            DmxFqStop(FqId);

            FqInfo = &DmxMgr->DmxFqInfo[FqId];

            str         = (HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) ? "Aud" : "Vid";
            BlockSize   = (HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) ? DMX_FQ_AUD_BLOCK_SIZE : DMX_FQ_VID_BLOCK_SIZE;

            if ((BufSize / DMX_MAX_AVFQ_DESC) > BlockSize)
            {
                BlockSize = (BufSize/ DMX_MAX_AVFQ_DESC) - ((BufSize / DMX_MAX_AVFQ_DESC) % 4);
                BlockSize = (BlockSize > DMX_MAX_BLOCK_SIZE) ? DMX_MAX_BLOCK_SIZE : BlockSize;
            }

            FqDepth     = BufSize / BlockSize + 1;
            FqBufSize   = FqDepth * DMX_FQ_DESC_SIZE;
            FqBufSize   = (FqBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;

            OqDepth     = (FqDepth < DMX_OQ_DEPTH) ? (FqDepth - 1) : DMX_OQ_DEPTH;
            OqBufSize   = OqDepth * DMX_OQ_DESC_SIZE;

            snprintf(BufName, 16,"DMX_%sBuf[%u]", str, ChanInfo->ChanId);

            ret = HI_DRV_MMZ_AllocAndMap(BufName, MMZ_OTHERS, BufSize + FqBufSize + OqBufSize, 0, &MmzBuf);
            if (HI_SUCCESS != ret)
            {
                HI_ERR_DEMUX("memory allocate failed, BufSize=0x%x\n", BufSize + FqBufSize + OqBufSize);

                DmxFqIdRelease(FqId);

                ChanInfo->DmxId = DMX_INVALID_DEMUX_ID;

                mutex_lock(&DmxMgr->lock_AVChan);
                --DmxMgr->AVChanCount;
                mutex_unlock(&DmxMgr->lock_AVChan);

                return HI_ERR_DMX_ALLOC_MEM_FAILED;
            }

            FqInfo->u32BufVirAddr   = MmzBuf.u32StartVirAddr;
            FqInfo->u32BufPhyAddr   = MmzBuf.u32StartPhyAddr;
            FqInfo->u32BufSize      = BufSize;
            FqInfo->u32BlockSize    = BlockSize;

            FqInfo->u32FQVirAddr    = FqInfo->u32BufVirAddr + BufSize;
            FqInfo->u32FQPhyAddr    = FqInfo->u32BufPhyAddr + BufSize;
            FqInfo->u32FQDepth      = FqDepth;

            OqInfo->u32OQPhyAddr    = FqInfo->u32FQPhyAddr + FqBufSize;
            OqInfo->u32OQVirAddr    = FqInfo->u32FQVirAddr + FqBufSize;
            OqInfo->u32OQDepth      = OqDepth;
        }
        else
        {
            FqInfo = &DmxMgr->DmxFqInfo[FqId];

            OqDepth     = FqInfo->u32FQDepth < DMX_OQ_DEPTH ? (FqInfo->u32FQDepth - 1) : DMX_OQ_DEPTH;
            OqBufSize   = OqDepth * DMX_OQ_DESC_SIZE;

            snprintf(BufName, 16,"DMX_OqBuf[%u]", ChanInfo->ChanId);

            ret = HI_DRV_MMZ_AllocAndMap(BufName, MMZ_OTHERS, OqBufSize, 0, &MmzBuf);
            if (HI_SUCCESS != ret)
            {
                HI_ERR_DEMUX("memory allocate failed, BufSize=0x%x\n", OqBufSize);

                ChanInfo->DmxId = DMX_INVALID_DEMUX_ID;

                return HI_ERR_DMX_ALLOC_MEM_FAILED;
            }

            OqInfo->u32OQPhyAddr    = MmzBuf.u32StartPhyAddr;
            OqInfo->u32OQVirAddr    = MmzBuf.u32StartVirAddr;
            OqInfo->u32OQDepth      = OqDepth;
        }

        OqInfo->enOQBufMode     = DMX_OQ_MODE_PLAY;
        OqInfo->u32OQId         = ChanInfo->ChanId;
        OqInfo->u32AttachId     = ChanInfo->ChanId;
        OqInfo->pWatchWaitQueue = HI_NULL;
        OqInfo->u32FQId         = FqId;
        OqInfo->OqWakeUp        = HI_FALSE;

        init_waitqueue_head(&OqInfo->OqWaitQueue);

        ChanBuf->u32BufPhyAddr      = FqInfo->u32BufPhyAddr;
        ChanBuf->u32BufKerVirAddr   = FqInfo->u32BufVirAddr;
        ChanBuf->u32BufSize         = FqInfo->u32BufSize;
    }

    DmxSetChannel(ChanInfo->ChanId);

#ifdef DMX_REGION_SUPPORT
    ++DmxMgr->SubDevInfo[DmxId].DmxChanCount;
#endif

    s_u32DMXPesLenErr[ChanInfo->ChanId] = 0;
    s_u32DMXCRCErr[ChanInfo->ChanId]    = 0;
    s_u32DMXTEIErr[ChanInfo->ChanId]    = 0;
    s_u32DMXDropCnt[ChanInfo->ChanId]   = 0;
    s_u32DMXCCDiscErr[ChanInfo->ChanId] = 0;

#ifdef DMX_USE_ECM
    if (HI_UNF_DMX_CHAN_TYPE_ECM_EMM == ChanInfo->ChanType)
    {
        HI_DMX_SwNewChannel(ChanInfo->ChanId);
    }
#endif

    *ChanId = ChanInfo->ChanId;

    return HI_SUCCESS;
}

/* notify vdec that the es buffer has been released */
static HI_VOID DMX_ReportDmxBufRelease(HI_HANDLE handle)
{
    HI_S32 ret = HI_FAILURE;
    VDEC_EXPORT_FUNC_S *pVdecFunc = NULL;

    ret = HI_DRV_MODULE_GetFunction(HI_ID_VDEC, (HI_VOID**)&pVdecFunc);

    if ((HI_SUCCESS == ret) && (HI_NULL != pVdecFunc->pfnVDEC_ReportDmxBufRls))
    {
        ret = pVdecFunc->pfnVDEC_ReportDmxBufRls(handle);
        if (HI_SUCCESS != ret)
        {
            HI_WARN_DEMUX("Set VDEC_CID_INFORM_DEMUX_RELEASE Failed!\n");
        }
    }
    else
    {
        HI_WARN_DEMUX("Get pVfmwFunc Failed!\n");
    }
}
/********************************************************/


HI_S32 DMX_OsiDestroyChannel(HI_U32 ChanId)
{
    HI_U32          i;
    HI_S32          ret;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];
    HI_U32          DmxId       = ChanInfo->DmxId;

    CHECKDMXID(DmxId);

#ifdef DMX_USE_ECM
    if (DMXOsiGetChnSwFlag(ChanId))
    {
        HI_DMX_SwDestoryChannel(ChanId);
    }
#endif

    if (HI_UNF_DMX_CHAN_CLOSE != ChanInfo->ChanStatus)
    {
        ret = DMX_OsiCloseChannel(ChanId);
        if (HI_SUCCESS != ret)
        {
            HI_ERR_DEMUX("close chan %d failed 0x%x\n", ChanId, ret);

            return ret;
        }
    }

    // detatch all filters attatched to this channel
    if (ChanInfo->FilterCount)
    {
        HI_U32 RegionBegin = 0;
        HI_U32 RegionEnd   = DMX_FILTER_CNT;

    #ifdef DMX_REGION_SUPPORT
        RegionBegin = DMX_GET_REGION_ID(DmxId) * DMX_REGION_FILTER_COUNT;
        RegionEnd   = RegionBegin + DMX_REGION_FILTER_COUNT;
    #endif

        mutex_lock(&DmxMgr->lock_Filter);
        for (i = RegionBegin; i < RegionEnd; i++)
        {
            DMX_FilterInfo_S *Filter = &DmxMgr->DmxFilterInfo[i];

            if (ChanId == Filter->ChanId)
            {
                Filter->ChanId = DMX_INVALID_CHAN_ID;

                DmxHalDetachFilter(Filter->FilterId, ChanId);
            }
        }
        mutex_unlock(&DmxMgr->lock_Filter);

        ChanInfo->FilterCount = 0;
    }

    DMX_ReportDmxBufRelease(DMX_CHANHANDLE(ChanId));

#ifdef DMX_DESCRAMBLER_SUPPORT
    //2. TODO detatch all keys attatched to this channel
    if (DMX_INVALID_KEY_ID != ChanInfo->KeyId)
    {
    	if ( g_pDscFunc != NULL )
		{
		    g_pDscFunc->DMX_OsiDescramblerDetach(ChanInfo->KeyId, ChanId);
			ChanInfo->KeyId = DMX_INVALID_KEY_ID;
		}
		else
		{
		    HI_ERR_DEMUX("error:g_pDscFunc have not beed registered\n");
		}
    }
#endif

    //3.delete the buffer from the channel
    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC == (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC & ChanInfo->ChanOutMode))
    {
        DmxHalSetChannelRecDmxid(ChanId, DMX_INVALID_DEMUX_ID);
    }

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
        DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[ChanInfo->ChanOqId];
        MMZ_BUFFER_S    MmzBuf;

        //3.1 Disable buffer
        DmxHalSetChannelPlayBufId(ChanId, 0);

        if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
        {
            DMX_FQ_Info_S *FqInfo = &DmxMgr->DmxFqInfo[OqInfo->u32FQId];

            MmzBuf.u32StartPhyAddr = FqInfo->u32BufPhyAddr;
            MmzBuf.u32StartVirAddr = FqInfo->u32BufVirAddr;

            DmxFqIdRelease(OqInfo->u32FQId);

            mutex_lock(&DmxMgr->lock_AVChan);
            --DmxMgr->AVChanCount;
            mutex_unlock(&DmxMgr->lock_AVChan);
        }
        else
        {
            MmzBuf.u32StartPhyAddr = OqInfo->u32OQPhyAddr;
            MmzBuf.u32StartVirAddr = OqInfo->u32OQVirAddr;
        }

        HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);
    }

    if (HI_UNF_DMX_CHAN_TYPE_POST == ChanInfo->ChanType)
    {
        DmxHalSetChannelTsPostMode(ChanId, DMX_DISABLE);
    }

    DmxHalSetChannelPid(ChanId, DMX_INVALID_PID);

    mutex_lock(&DmxMgr->lock_Channel);

    ChanInfo->DmxId     = DMX_INVALID_DEMUX_ID;
    ChanInfo->ChanId    = DMX_INVALID_CHAN_ID;
    ChanInfo->ChanPid   = DMX_INVALID_PID;

#ifdef DMX_REGION_SUPPORT
    --DmxMgr->SubDevInfo[DmxId].DmxChanCount;
#endif

    mutex_unlock(&DmxMgr->lock_Channel);

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiOpenChannel
* Description   : open a channel and start to work
* Input         : ChanId
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiOpenChannel(HI_U32 ChanId)
{
    DMX_DEV_OSI_S      *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S     *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_OQ_Info_S      *OqInfo;
    DMX_Sub_DevInfo_S  *DmxInfo;

    ChanInfo = &pDmxDevOsi->DmxChanInfo[ChanId];

    CHECKDMXID(ChanInfo->DmxId);

    OqInfo  = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
    DmxInfo = &pDmxDevOsi->SubDevInfo[ChanInfo->DmxId];

    if ((DMX_PORT_MODE_TUNER != DmxInfo->PortMode) && (DMX_PORT_MODE_RAM != DmxInfo->PortMode))
    {
        HI_ERR_DEMUX("not attach port\n");
        return HI_ERR_DMX_NOATTACH_PORT;
    }

    if (HI_UNF_DMX_CHAN_CLOSE != ChanInfo->ChanStatus)
    {
        HI_WARN_DEMUX("channel %d is already opened\n", ChanId);
        return HI_SUCCESS;
    }

    //0. Reset channel counter
    DmxHalResetChannelCounter(ChanId);

    //1. if ch is used for play
    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode)
    {
        DmxHalSetChannelPlayDmxid(ChanId, ChanInfo->DmxId);

        if ((ChanInfo->ChanType != HI_UNF_DMX_CHAN_TYPE_SEC) && (ChanInfo->ChanType != HI_UNF_DMX_CHAN_TYPE_ECM_EMM))
        {
            DmxHalSetChannelFltMode(ChanId, DMX_DISABLE);
            if (ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_PES)
            {
                ChanInfo->u32PesBlkCnt = 0;
            }
        }
        else
        {
            DmxHalSetChannelFltMode(ChanId, DMX_ENABLE);
        }

        if ((ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_AUD) || (ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_VID))
        {
            DMX_Sub_DevInfo_S *DmxInfo = &pDmxDevOsi->SubDevInfo[ChanInfo->DmxId];

            if ((DMX_PORT_MODE_RAM == DmxInfo->PortMode) && (DmxInfo->PortId < DMX_RAMPORT_CNT))
            {
                //open aud and vid fq bp
                DmxHalAttachIPBPFQ(DmxInfo->PortId, OqInfo->u32FQId);
            }
        }
    }

    //2. if ch is used for rec
    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC & ChanInfo->ChanOutMode)
    {
        DMX_RecInfo_S *RecInfo = &pDmxDevOsi->DmxRecInfo[ChanInfo->DmxId];

        if (HI_UNF_DMX_REC_TYPE_SELECT_PID == RecInfo->RecType)
        {
            DmxHalSetChannelRecBufId(ChanId, RecInfo->RecOqId);
        }

        DmxHalSetChannelRecDmxid(ChanId, ChanInfo->DmxId);
    }

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC & ChanInfo->ChanOutMode)
    {
        ChanInfo->ChanStatus |= HI_UNF_DMX_CHAN_REC_EN;
    }

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode)
    {
        DmxOqStart(ChanInfo->ChanOqId);

        if ((ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_AUD) || (ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_VID))
        {
            DmxFqStart(OqInfo->u32FQId);
        }

        ChanInfo->ChanStatus |= HI_UNF_DMX_CHAN_PLAY_EN;
    }

    ChanInfo->u32AcqTimeInterval = 0;
    ChanInfo->u32RelTimeInterval = 0;

    HI_DRV_SYS_GetTimeStampMs(&ChanInfo->u32AcqTime);
    ChanInfo->u32RelTime = ChanInfo->u32AcqTime;

#ifdef DMX_USE_ECM
    if (DMXOsiGetChnSwFlag(ChanId))
    {
        HI_DMX_SwOpenChannel(ChanId);
    }
#endif

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiCloseChannel
* Description   : close a channel and stop working
* Input         : ChanId
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiCloseChannel(HI_U32 ChanId)
{
    DMX_DEV_OSI_S      *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S     *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_OQ_Info_S      *OqInfo;
    DMX_FLUSH_TYPE_E    FlushType;

    CHECKDMXID(ChanInfo->DmxId);

    OqInfo = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];

    ChanInfo->ChanStatus    = HI_UNF_DMX_CHAN_CLOSE;
    ChanInfo->u32TotolAcq   = 0;
    ChanInfo->u32HitAcq     = 0;
    ChanInfo->u32Release    = 0;
    ChanInfo->u32PesLength  = 0;
    ChanInfo->ChanEosFlag   = HI_FALSE;

    // close channel
    DmxHalSetChannelPlayDmxid(ChanId, DMX_INVALID_DEMUX_ID);

#ifdef DMX_USE_ECM
    if (DMXOsiGetChnSwFlag(ChanId))
    {
        HI_DMX_SwCloseChannel(ChanId);
    }
#endif

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
        if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
        {
            DmxFqStop(OqInfo->u32FQId);
        }
    }

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == ChanInfo->ChanOutMode)
    {
        FlushType = DMX_FLUSH_TYPE_REC_PLAY;
    }
    else if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC == ChanInfo->ChanOutMode)
    {
        FlushType = DMX_FLUSH_TYPE_REC;
    }
    else
    {
        FlushType = DMX_FLUSH_TYPE_REC_PLAY;
    }

    DMX_OsiResetChannel(ChanId, FlushType);
    
    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
       DmxOqStop(ChanInfo->ChanOqId);

        if ((HI_UNF_DMX_CHAN_TYPE_AUD == ChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == ChanInfo->ChanType))
        {
            DMX_Sub_DevInfo_S *DmxInfo = &pDmxDevOsi->SubDevInfo[ChanInfo->DmxId];

            if ((DMX_PORT_MODE_RAM == DmxInfo->PortMode) && (DmxInfo->PortId < DMX_RAMPORT_CNT))
            {
                DmxHalDetachIPBPFQ(DmxInfo->PortId, OqInfo->u32FQId);
                DmxHalClrIPFqBPStatus(DmxInfo->PortId, OqInfo->u32FQId);
            }
        } 
    }

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      : DMX_OsiGetChannelAttr
* Description   : Get Channel Attr
* Input         : ChanId, ChanAttr
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiGetChannelAttr(HI_U32 ChanId, HI_UNF_DMX_CHAN_ATTR_S *ChanAttr)
{
    HI_S32          ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if (0 == mutex_lock_interruptible(&DmxMgr->lock_Channel))
    {
        if (ChanInfo->DmxId < DMX_CNT)
        {
            ChanAttr->u32BufSize    = ChanInfo->ChanBufSize;
            ChanAttr->enChannelType = ChanInfo->ChanType;
            ChanAttr->enCRCMode     = ChanInfo->ChanCrcMode;
            ChanAttr->enOutputMode  = ChanInfo->ChanOutMode;

            ret = HI_SUCCESS;
        }
        mutex_unlock(&DmxMgr->lock_Channel);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

/***********************************************************************************
* Function      : DMX_OsiSetChannelAttr
* Description   : Set Channel Attr
* Input         : ChanId, ChanAttr
* Output        :
* Return        : HI_SUCCESS:     success
*                 HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiSetChannelAttr(HI_U32 ChanId, HI_UNF_DMX_CHAN_ATTR_S *ChanAttr)
{
    HI_S32          ret         = HI_SUCCESS;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    CHECKDMXID(ChanInfo->DmxId);

    if (   (ChanAttr->u32BufSize    != ChanInfo->ChanBufSize)
        || (ChanAttr->enChannelType != ChanInfo->ChanType)
        || (ChanAttr->enOutputMode  != ChanInfo->ChanOutMode) )
    {
        HI_ERR_DEMUX("do not suppor change attr of buffer size, channel type and channel output mode!");
        return HI_ERR_DMX_NOT_SUPPORT;
    }

    switch (ChanAttr->enChannelType)
    {
        case HI_UNF_DMX_CHAN_TYPE_SEC :
        case HI_UNF_DMX_CHAN_TYPE_ECM_EMM :
            switch (ChanAttr->enCRCMode)
            {
                case HI_UNF_DMX_CHAN_CRC_MODE_FORBID :
                case HI_UNF_DMX_CHAN_CRC_MODE_FORCE_AND_DISCARD :
                case HI_UNF_DMX_CHAN_CRC_MODE_FORCE_AND_SEND:
                case HI_UNF_DMX_CHAN_CRC_MODE_BY_SYNTAX_AND_DISCARD :
                case HI_UNF_DMX_CHAN_CRC_MODE_BY_SYNTAX_AND_SEND :
                {
                    ChanInfo->ChanCrcMode = ChanAttr->enCRCMode;

                    DmxHalSetChannelCRCMode(ChanInfo->ChanId, ChanInfo->ChanCrcMode);

                    break;
                }

                default :
                    HI_ERR_DEMUX("invalid crc mode %u\n", ChanAttr->enCRCMode);

                    ret = HI_ERR_DMX_INVALID_PARA;
            }

            break;

        case HI_UNF_DMX_CHAN_TYPE_PES :
        case HI_UNF_DMX_CHAN_TYPE_AUD :
        case HI_UNF_DMX_CHAN_TYPE_VID :
        case HI_UNF_DMX_CHAN_TYPE_POST :
            if (ChanAttr->enCRCMode != ChanInfo->ChanCrcMode)
            {
                HI_ERR_DEMUX("PES/VID/AUD/POST channels do not support change their crc mode: %u\n", ChanAttr->enCRCMode);
                ret = HI_ERR_DMX_NOT_SUPPORT;
            }

            break;

        default :
            HI_ERR_DEMUX("invalid chan type %u\n", ChanAttr->enChannelType);

            ret = HI_ERR_DMX_INVALID_PARA;
    }

    return ret;
}

HI_S32 DMX_OsiGetChannelPid(HI_U32 ChanId, HI_U32 *Pid)
{
    HI_S32          ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if (ChanInfo->DmxId < DMX_CNT)
    {
        *Pid = ChanInfo->ChanPid;

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiSetChannelPid(HI_U32 ChanId, HI_U32 Pid)
{
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if ((ChanInfo->DmxId >= DMX_CNT) || (Pid > DMX_INVALID_PID))
    {
        HI_ERR_DEMUX("invalid param, dmxid:%d,Pid:0x%x\n",ChanInfo->DmxId, Pid);
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (HI_UNF_DMX_CHAN_CLOSE != ChanInfo->ChanStatus)
    {
        HI_ERR_DEMUX("close channel first when set channel pid!\n");
        return HI_ERR_DMX_OPENING_CHAN;
    }

    if (Pid < DMX_INVALID_PID)
    {
        HI_U32 i;

        for (i = 0; i < DMX_CHANNEL_CNT; i++)
        {
            if ((DmxMgr->DmxChanInfo[i].DmxId == ChanInfo->DmxId) && (i != ChanId))
            {
                // the same dmx, Pid has been occupied by other channel
                if (DmxMgr->DmxChanInfo[i].ChanPid == Pid)
                {
                    DmxHalSetChannelPid(DmxMgr->DmxChanInfo[i].ChanId,DMX_INVALID_PID);
                    DmxMgr->DmxChanInfo[i].ChanPid = DMX_INVALID_PID;
                    HI_ERR_DEMUX("Attention: occupied channel pid on same DMX,already changed the pid of channel %u\n", 
                                 DmxMgr->DmxChanInfo[i].ChanId);
                    break;
                }
            }
        }
    }

    ChanInfo->ChanPid = Pid;
    DmxHalSetChannelPid(ChanId, Pid);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiGetChannelStatus(HI_U32 ChanId, HI_UNF_DMX_CHAN_STATUS_E *ChanStatus)
{
    HI_S32          ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if (0 == mutex_lock_interruptible(&DmxMgr->lock_Channel))
    {
        if (ChanInfo->DmxId < DMX_CNT)
        {
            *ChanStatus = ChanInfo->ChanStatus;

            ret = HI_SUCCESS;
        }
        mutex_unlock(&DmxMgr->lock_Channel);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

HI_S32 DMX_OsiGetFreeChannelNum(HI_U32 DmxId, HI_U32 *FreeCount)
{
    DMX_ChanInfo_S *ChanInfo    = g_pDmxDevOsi->DmxChanInfo;
    HI_U32          RegionBegin = 0;
    HI_U32          RegionEnd   = DMX_CHANNEL_CNT;
    HI_U32          i;

    *FreeCount = 0;

#ifdef DMX_REGION_SUPPORT
    RegionBegin = DMX_GET_REGION_ID(DmxId) * DMX_REGION_CHANNEL_COUNT;
    RegionEnd   = RegionBegin + DMX_REGION_CHANNEL_COUNT;
#endif

    for (i = RegionBegin; i < RegionEnd; i++)
    {
        if (ChanInfo[i].DmxId >= DMX_CNT)
        {
            ++(*FreeCount);
        }
    }

    return HI_SUCCESS;
}

/***********************************************************************************
* Function      :   DMX_OsiGetChannelScrambleFlag
* Description   :  get channel scrambe flag
* Input         :  u32ChannelId ,
* Output        :  penScrambleFlag
* Return        :  HI_SUCCESS:     success
*                  HI_FAILURE:
* Others:
***********************************************************************************/
HI_S32 DMX_OsiGetChannelScrambleFlag(HI_U32 u32ChannelId, HI_UNF_DMX_SCRAMBLED_FLAG_E *penScrambleFlag)
{
    HI_BOOL bTSScramble, bPesScramble;
    DMX_ChanInfo_S *pChInofo;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;

    IsDmxDevInit();

    pChInofo = &pDmxDevOsi->DmxChanInfo[u32ChannelId];

    CHECKDMXID(pChInofo->DmxId);

    if (HI_UNF_DMX_CHAN_CLOSE == pChInofo->ChanStatus)
    {
        HI_ERR_DEMUX("channel %d is not opened!\n", u32ChannelId);
        return HI_ERR_DMX_NOT_OPEN_CHAN;
    }

    DmxHalGetChannelTSScrambleFlag(u32ChannelId, &bTSScramble);
    DmxHalGetChannelPesScrambleFlag(u32ChannelId, &bPesScramble);
    if (HI_TRUE == bTSScramble)
    {
        *penScrambleFlag = HI_UNF_DMX_SCRAMBLED_FLAG_TS;
    }
    else if (HI_TRUE == bPesScramble)
    {
        *penScrambleFlag = HI_UNF_DMX_SCRAMBLED_FLAG_PES;
    }
    else
    {
        *penScrambleFlag = HI_UNF_DMX_SCRAMBLED_FLAG_NO;
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiSetChannelEosFlag(HI_U32 ChanId)
{
    DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[ChanId];

    CHECKDMXID(ChanInfo->DmxId);

    if (HI_UNF_DMX_CHAN_CLOSE == ChanInfo->ChanStatus)
    {
        HI_ERR_DEMUX("channel %d is not opened!\n", ChanId);
        return HI_ERR_DMX_NOT_OPEN_CHAN;
    }

    ChanInfo->ChanEosFlag = HI_TRUE;

    return HI_SUCCESS;
}

#ifdef DMX_USE_ECM
HI_S32 DMX_OsiGetChannelSwFlag(HI_U32 u32ChannelId, HI_U32* pu32SwFlag)
{
    *pu32SwFlag = DMXOsiGetChnSwFlag(u32ChannelId);
    return HI_SUCCESS;
}

HI_S32 DMX_OsiGetChannelSwBufAddr(HI_U32 u32ChannelId, MMZ_BUFFER_S* pstSwBuf)
{
    return HI_DMX_SwGetChannelSwBufAddr(u32ChannelId, pstSwBuf);
}
#endif

HI_S32 DMX_OsiGetChannelTsCnt(HI_U32 ChanId, HI_U32 *pTsCnt)
{
    DMX_ChanInfo_S *pChanInfo;
    DMX_DEV_OSI_S  *pDmxDevOsi = g_pDmxDevOsi;

    IsDmxDevInit();

    pChanInfo = &pDmxDevOsi->DmxChanInfo[ChanId];
    if (pChanInfo->DmxId >= DMX_CNT)
    {
        HI_ERR_DEMUX("channel %d is no existed\n", ChanId);

        return HI_ERR_DMX_INVALID_PARA;
    }

    *pTsCnt = DmxHalGetChannelCounter(ChanId);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiSetChannelCCRepeat(HI_U32 ChanId, HI_UNF_DMX_CHAN_CC_REPEAT_SET_S * pstChCCReaptSet)
{
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S *pChanInfo;

    IsDmxDevInit();
    
    CHECKPOINTER(pstChCCReaptSet);
    
    pChanInfo = &pDmxDevOsi->DmxChanInfo[ChanId];
    CHECKDMXID(pChanInfo->DmxId);

    if (pstChCCReaptSet->enCCRepeatMode == HI_UNF_DMX_CHAN_CC_REPEAT_MODE_DROP)
    {
        DmxHalSetChannelCCRepeatCtrl(ChanId, DMX_DISABLE);
    }
    else
    {
        DmxHalSetChannelCCRepeatCtrl(ChanId, DMX_ENABLE);
    }

    return HI_SUCCESS;
}


HI_S32 DMX_OsiGetChannelId(HI_U32 DmxId, HI_U32 Pid, HI_U32 *ChanId)
{
    HI_S32          ret         = HI_ERR_DMX_UNMATCH_CHAN;
    DMX_DEV_OSI_S  *DmxDevOsi   = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = DmxDevOsi->DmxChanInfo;
    HI_U32          i;

    if (Pid >= DMX_INVALID_PID)
    {
        HI_ERR_DEMUX("invalid pid:%d!\n", Pid);
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (0 == mutex_lock_interruptible(&DmxDevOsi->lock_Channel))
    {
        for (i = 0; i < DMX_CHANNEL_CNT; i++)
        {
            if ((ChanInfo[i].DmxId == DmxId) && (ChanInfo[i].ChanPid == Pid))
            {
                *ChanId = ChanInfo[i].ChanId;

                ret = HI_SUCCESS;

                break;
            }
        }
        mutex_unlock(&DmxDevOsi->lock_Channel);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;
    }

    return ret;
}

HI_S32  DMX_OsiReadDataRequset(HI_U32         u32ChId,
                               HI_U32         u32AcqNum,
                               HI_U32 *       pu32AcqedNum,
                               DMX_UserMsg_S* psMsgList,
                               HI_U32         u32TimeOutMs)
{
    DMX_ChanInfo_S *pChanInfo;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_OQ_Info_S  *OqInfo;
    HI_S32 s32Ret;

    IsDmxDevInit();

    if (0 == u32AcqNum)
    {
        HI_ERR_DEMUX(" channel %d acquire data num = 0!\n", u32ChId);
        return HI_ERR_DMX_INVALID_PARA;
    }

    pChanInfo = &pDmxDevOsi->DmxChanInfo[u32ChId];
#ifdef DMX_USE_ECM
    if (!pChanInfo->u32SwFlag)
#endif
    {
        HI_U32 TimeMs= 0;

        pChanInfo->u32TotolAcq++;

        HI_DRV_SYS_GetTimeStampMs(&TimeMs);

        pChanInfo->u32AcqTimeInterval = TimeMs - pChanInfo->u32AcqTime;
        pChanInfo->u32AcqTime = TimeMs;
    }

    if (HI_UNF_DMX_CHAN_PLAY_EN != (HI_UNF_DMX_CHAN_PLAY_EN & pChanInfo->ChanStatus))
    {
        HI_ERR_DEMUX("channel %d is not open yet or not play channel\n", u32ChId);

        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }

    if (   (HI_UNF_DMX_CHAN_TYPE_SEC    == pChanInfo->ChanType)
        || (HI_UNF_DMX_CHAN_TYPE_ECM_EMM== pChanInfo->ChanType)
        || (HI_UNF_DMX_CHAN_TYPE_POST   == pChanInfo->ChanType) )
    {
        pChanInfo->u32ChnResetLock = 1;
        OqInfo  = &g_pDmxDevOsi->DmxOqInfo[pChanInfo->ChanOqId];
        s32Ret = DMXOsiReadSec(pChanInfo, u32AcqNum, pu32AcqedNum, psMsgList, OqInfo, u32TimeOutMs);
#ifdef DMX_USE_ECM
        if (pChanInfo->ChanStatus == HI_UNF_DMX_CHAN_CLOSE && pChanInfo->u32SwFlag)
        {
            s32Ret = DMX_OsiCloseChannel(pChanInfo->ChanId);//reset channel again
            if (HI_SUCCESS != s32Ret)
            {
                HI_ERR_DEMUX("close channel error:%x!\n",s32Ret);
            }
            s32Ret = HI_DMX_SwOpenChannel(u32ChId);
            if (HI_SUCCESS != s32Ret)
            {
                HI_ERR_DEMUX("open sw channel error:%x!\n",s32Ret);
            }
            s32Ret = DMX_OsiOpenChannel(u32ChId);
            if (HI_SUCCESS != s32Ret)
            {
                HI_ERR_DEMUX("open sw channel error:%x!\n",s32Ret);
            }
            pChanInfo->u32ChnResetLock = 0;
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }
#endif
        pChanInfo->u32ChnResetLock = 0;
        return s32Ret;
    }
    else if (HI_UNF_DMX_CHAN_TYPE_PES == pChanInfo->ChanType)
    {
#if 0
        if (u32AcqNum < DMX_PES_MAX_SIZE)
        {
            HI_ERR_DEMUX("For Channel type DMX_CHAN_PES,we stronly recommend you to set the param u32AcquireNum of api HI_UNF_DMX_AcquireBuf >= %d !\n",
                         u32ChId);
            return HI_ERR_DMX_INVALID_PARA;
        }

#else
        u32AcqNum = DMX_PES_MAX_SIZE;
#endif
        pChanInfo->u32ChnResetLock = 1;
        s32Ret = DMXOsiReadPes(pChanInfo, u32AcqNum, pu32AcqedNum, psMsgList, u32TimeOutMs);
        pChanInfo->u32ChnResetLock = 0;
        return s32Ret;
    }
    else
    {
        HI_ERR_DEMUX("channel %d is wrong channeltype %d!\n", u32ChId, pChanInfo->ChanType);

        return HI_ERR_DMX_INVALID_PARA;
    }
}

/**
 \brief
 \attention

 \param[in] u32ChId

 \retval none
 \return none

 \see
 \li ::
 */
HI_U32 DMX_OsiGetChType(HI_U32 u32ChId)
{
    DMX_ChanInfo_S *pChanInfo;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;

    IsDmxDevInit();
    pChanInfo = &pDmxDevOsi->DmxChanInfo[u32ChId];
    return pChanInfo->ChanType;
}

HI_S32 DMX_OsiReleaseReadData(HI_U32 u32ChId, HI_U32 u32RelNum, DMX_UserMsg_S* psMsgList)
{
    DMX_DEV_OSI_S  *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S *pChanInfo;
    DMX_OQ_Info_S  *OqInfo;
    HI_U32 u32Fq, u32Blkcnt = 0;
    HI_U32 *pu32ReleaseBlk, u32WriteBlk, u32ReadPtr;
    HI_U32 u32CurrentBlk, u32BufPhyAddr, u32BufSize, u32BufLen;
    HI_U32 u32ReadBlkNext, u32NextBlk, u32BufLenNext = 0;
    HI_U32 u32BufPhyAddrNext = 0;
    HI_U32 u32RealseFlag, u32NextBlkValidFlag;
    HI_U32 *pAddr = HI_NULL;
    HI_U32 i = 0;
    DMX_FQ_Info_S  *FqInfo;
    HI_SIZE_T u32LockFlag;

    pChanInfo = &pDmxDevOsi->DmxChanInfo[u32ChId];

    if (HI_UNF_DMX_CHAN_PLAY_EN != (HI_UNF_DMX_CHAN_PLAY_EN & pChanInfo->ChanStatus))
    {
        HI_ERR_DEMUX("channel %d is not open yet or not play channel\n", u32ChId);
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }
#ifdef DMX_USE_ECM
    if (!pChanInfo->u32SwFlag)
#endif
    {
        HI_U32 TimeMs = 0;

        pChanInfo->u32Release++;

        HI_DRV_SYS_GetTimeStampMs(&TimeMs);

        pChanInfo->u32RelTimeInterval = TimeMs - pChanInfo->u32RelTime;
        pChanInfo->u32RelTime = TimeMs;
    }

    OqInfo = &pDmxDevOsi->DmxOqInfo[pChanInfo->ChanOqId];

    pChanInfo->u32ChnResetLock = 1;
    DMXOsiOQGetReadWrite(pChanInfo->ChanOqId, &u32WriteBlk, &u32ReadPtr);

    pAddr = HI_KMALLOC(HI_ID_DEMUX, sizeof(HI_U32) * DMX_MAX_ERRLIST_NUM * 2, GFP_KERNEL);
    if (HI_NULL == pAddr)
    {
        HI_FATAL_DEMUX("Malloc pAddr failed\n");
        pChanInfo->u32ChnResetLock = 0;
        return HI_FAILURE;
    }

    OqInfo->u32ReleaseBlk = u32ReadPtr;
    pu32ReleaseBlk = &OqInfo->u32ReleaseBlk;

    if (   (HI_UNF_DMX_CHAN_TYPE_SEC    == pChanInfo->ChanType)
        || (HI_UNF_DMX_CHAN_TYPE_ECM_EMM== pChanInfo->ChanType)
        || (HI_UNF_DMX_CHAN_TYPE_POST   == pChanInfo->ChanType) )
    {
        while (*pu32ReleaseBlk != u32WriteBlk)
        {
            u32RealseFlag = 0;

            u32CurrentBlk = OqInfo->u32OQVirAddr + *pu32ReleaseBlk * DMX_OQ_DESC_SIZE;
            u32BufPhyAddr = *(HI_U32 *)u32CurrentBlk;
            u32BufLen  = *(HI_U32 *)(u32CurrentBlk + 8) & 0xffff;
            u32BufSize = *(HI_U32 *)(u32CurrentBlk + 4) & 0xffff;
            u32ReadBlkNext = *pu32ReleaseBlk + 1;
            if (u32ReadBlkNext >= OqInfo->u32OQDepth)
            {
                u32ReadBlkNext = 0;
            }

            if (IsAddrValid(u32ReadPtr, u32WriteBlk, u32ReadBlkNext) != HI_SUCCESS)
            {
                u32NextBlkValidFlag = 0;
            }
            else
            {
                DmxHalGetOQWORDx(pChanInfo->ChanOqId, DMX_OQ_SADDR_OFFSET, &u32BufPhyAddrNext);
                DmxHalGetOQWORDx(pChanInfo->ChanOqId, DMX_OQ_SZUS_OFFSET, &u32BufLenNext);
                u32BufLenNext >>= 16;
                DMXOsiOQGetReadWrite(pChanInfo->ChanOqId, &u32WriteBlk, HI_NULL);
                if (u32ReadBlkNext == u32WriteBlk)
                {
                    u32NextBlkValidFlag = 1;
                }
                else
                {
                    while(1)
                    {
                        if (u32ReadBlkNext != u32WriteBlk)
                        {
                            u32NextBlk = OqInfo->u32OQVirAddr + u32ReadBlkNext * DMX_OQ_DESC_SIZE;
                            u32BufPhyAddrNext = *(HI_U32 *)u32NextBlk;
                            u32BufLenNext = *(HI_U32 *)(u32CurrentBlk + 4) & 0xffff; //ues buf size instead of buf len,for next block may invalid
                            if(DMXOsiCheckErrMSG(u32BufPhyAddrNext) == 0)
                            {
                                u32NextBlkValidFlag = 1;
                                break;
                            }
                            else
                            {
                                DMXINC(u32ReadBlkNext, OqInfo->u32OQDepth);
                                continue;
                            }
                        }
                        else
                        {
                            DmxHalGetOQWORDx(pChanInfo->ChanOqId, DMX_OQ_SADDR_OFFSET, &u32BufPhyAddrNext);
                            DmxHalGetOQWORDx(pChanInfo->ChanOqId, DMX_OQ_SZUS_OFFSET, &u32BufLenNext);
                            u32BufLenNext >>= 16;
                            u32NextBlkValidFlag = 1;
                            break;
                        }
                    }
                }
            }

            for (; i < u32RelNum; i++)
            {
                u32RealseFlag = 0;
                if ((psMsgList[i].u32BufStartAddr >= u32BufPhyAddr)
                   && (psMsgList[i].u32BufStartAddr <= (u32BufPhyAddr + u32BufLen - 1)))
                {
                    if ((psMsgList[i].u32BufStartAddr + psMsgList[i].u32MsgLen) >= (u32BufPhyAddr + u32BufLen))
                    {
                        u32RealseFlag = 1;
                    }
                }
                else if (u32NextBlkValidFlag)
                {
                    if ((psMsgList[i].u32BufStartAddr >= u32BufPhyAddrNext)
                       && (psMsgList[i].u32BufStartAddr <= (u32BufPhyAddrNext + u32BufLenNext - 1)))
                    {
                        u32RealseFlag = 1;
                    }
                    else
                    {
                        if (DMXOsiRelErrMSG(u32BufPhyAddr) == 0)
                        {
                            //u32RealseFlag = 1;
                            if (++ * pu32ReleaseBlk >= OqInfo->u32OQDepth)
                            {
                                *pu32ReleaseBlk = 0;
                            }

                            pAddr[2 * u32Blkcnt + 0] = u32BufPhyAddr;
                            pAddr[2 * u32Blkcnt + 1] = u32BufSize;
                            u32Blkcnt++;
                            break;
                        }
                        else
                        {
                            HI_ERR_DEMUX("psMsgList[%d].u32BufStartAddr:%x,len:%x,bufferAddr:%x,nextptr:%x,nexlen:%x,u32OQPhyAddr:%x,rel:%x,read:%x\n",
                                           i, psMsgList[i].u32BufStartAddr, psMsgList[i].u32MsgLen, u32BufPhyAddr,
                                         u32BufPhyAddrNext, u32BufLenNext, OqInfo->u32OQPhyAddr,
                                         *pu32ReleaseBlk,u32ReadPtr);
                            HI_KFREE(HI_ID_DEMUX, (HI_VOID *) pAddr);
                            pChanInfo->u32ChnResetLock = 0;                            
                            /*pay attentaion ,this is a bug ,we just call ResetPoolBuf to resume the poolbuf 
                            to avoid some terriable things appears*/
                            ResetPoolBuf();
                            return HI_ERR_DMX_INVALID_PARA;
                        }
                    }
                }

                if (u32RealseFlag)
                {
                    if (++ * pu32ReleaseBlk >= OqInfo->u32OQDepth)
                    {
                        *pu32ReleaseBlk = 0;
                    }

                    DMXOsiRelErrMSG(u32BufPhyAddr);
                    pAddr[2 * u32Blkcnt + 0] = u32BufPhyAddr;
                    pAddr[2 * u32Blkcnt + 1] = u32BufSize;
                    u32Blkcnt++;
                    i++;
                    break;
                }
            }

            if (i == u32RelNum)
            {
                break;
            }
        }

        if (u32Blkcnt)
        {
            u32Fq = OqInfo->u32FQId;

            FqInfo = &pDmxDevOsi->DmxFqInfo[u32Fq];

            spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);

            DmxHalSetOQReadPtr(pChanInfo->ChanOqId, *pu32ReleaseBlk);

            u32WriteBlk = DmxHalGetFQWritePtr(u32Fq);

            for (i = 0; i < u32Blkcnt; i++)
            {
                u32CurrentBlk = pDmxDevOsi->DmxFqInfo[u32Fq].u32FQVirAddr + u32WriteBlk * DMX_FQ_DESC_SIZE;
                *(HI_U32 *)u32CurrentBlk = pAddr[2 * i + 0];
                *(HI_U32 *)(u32CurrentBlk + 4) = pAddr[2 * i + 1];
                if ((++u32WriteBlk) >= pDmxDevOsi->DmxFqInfo[u32Fq].u32FQDepth)
                {
                    u32WriteBlk = 0;
                }
            }

            DmxHalSetFQWritePtr(u32Fq, u32WriteBlk);

            spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);

            DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[pChanInfo->DmxId], u32Fq);

            DmxFqCheckOverflowInt(u32Fq);

            //DMXCheckOQEnableStatus(pChanInfo->stChBuf.u32OQId, pChanInfo->stChBuf.u32OQDepth);
            if (*pu32ReleaseBlk < u32WriteBlk)
            {
                if (OqInfo->u32ProcsBlk < *pu32ReleaseBlk || OqInfo->u32ProcsBlk > u32WriteBlk)//invalid procs ptr
                {
                    OqInfo->u32ProcsBlk = *pu32ReleaseBlk;
                    OqInfo->u32ProcsOffset = 0;
                }
            }
            else
            {
                if (OqInfo->u32ProcsBlk > u32WriteBlk && OqInfo->u32ProcsBlk < *pu32ReleaseBlk)//invalid procs ptr
                {
                    OqInfo->u32ProcsBlk = *pu32ReleaseBlk;
                    OqInfo->u32ProcsOffset = 0;
                }
            }
        }
    }
    else if (HI_UNF_DMX_CHAN_TYPE_PES == pChanInfo->ChanType)
    {
        for (; i < u32RelNum && u32ReadPtr != u32WriteBlk; i++)
        {
            u32CurrentBlk = OqInfo->u32OQVirAddr + (u32ReadPtr) * DMX_OQ_DESC_SIZE;
            u32BufPhyAddr = *(HI_U32 *)u32CurrentBlk;
            u32BufSize = *(HI_U32 *)(u32CurrentBlk + 4) & 0xffff;
            if (psMsgList[i].u32BufStartAddr != u32BufPhyAddr)
            {
                HI_ERR_DEMUX("release addr: %x, oq read phyaddr: %x\n", psMsgList[i].u32BufStartAddr, u32BufPhyAddr);
                HI_KFREE(HI_ID_DEMUX, (HI_VOID *) pAddr);
                pChanInfo->u32ChnResetLock = 0;
                return HI_ERR_DMX_INVALID_PARA;
            }

            pAddr[2 * u32Blkcnt + 0] = u32BufPhyAddr;
            pAddr[2 * u32Blkcnt + 1] = u32BufSize;
            u32Blkcnt++;
            DMXINC(u32ReadPtr, OqInfo->u32OQDepth);
            if (u32ReadPtr == u32WriteBlk)
            {
                break;
            }
        }

        if (u32Blkcnt)
        {
            DmxHalSetOQReadPtr(pChanInfo->ChanOqId, u32ReadPtr);

            u32Fq = OqInfo->u32FQId;

            FqInfo = &pDmxDevOsi->DmxFqInfo[u32Fq];

            spin_lock_irqsave(&FqInfo->LockFq, u32LockFlag);

            u32WriteBlk = DmxHalGetFQWritePtr(u32Fq);

            for (i = 0; i < u32Blkcnt; i++)
            {
                u32CurrentBlk = pDmxDevOsi->DmxFqInfo[u32Fq].u32FQVirAddr + u32WriteBlk * DMX_FQ_DESC_SIZE;
                *(HI_U32 *)u32CurrentBlk = pAddr[2 * i + 0];
                *(HI_U32 *)(u32CurrentBlk + 4) = pAddr[2 * i + 1];
                if ((++u32WriteBlk) >= pDmxDevOsi->DmxFqInfo[u32Fq].u32FQDepth)
                {
                    u32WriteBlk = 0;
                }
            }

            DmxHalSetFQWritePtr(u32Fq, u32WriteBlk);

            spin_unlock_irqrestore(&FqInfo->LockFq, u32LockFlag);

            DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[pChanInfo->DmxId], u32Fq);
            //DMXCheckOQEnableStatus(pChanInfo->stChBuf.u32OQId, pChanInfo->stChBuf.u32OQDepth);

            DmxFqCheckOverflowInt(u32Fq);
        }
    }

    HI_KFREE(HI_ID_DEMUX, (HI_VOID *) pAddr);
    pChanInfo->u32ChnResetLock = 0;
    return HI_SUCCESS;
}

static HI_S32 DmxGetPesHeaderLen(HI_U8 *buf, HI_U32 len, HI_U32 *PesHeaderLen)
{
    HI_S32 ret = HI_FAILURE;

    *PesHeaderLen = 0;

    if (len >= DMX_PES_HEADER_LENGTH)
    {
        *PesHeaderLen = DMX_PES_HEADER_LENGTH + buf[8];

        ret = HI_SUCCESS;
    }

    return ret;
}
/*
 * This is used to peek the first u32PeekLen of a SEC or PES packet. It does not consume the data in
 * the internal demux section buffer, i.e. it simply copies the requested amount of data
 * but does not move the consumer pointer.
 */
HI_S32 DMX_OsiPeekDataRequest(HI_U32         u32ChId,
                              HI_U32         u32PeekLen,
                              DMX_UserMsg_S* psMsgList)
{
    DMX_ChanInfo_S *pChanInfo;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S stTmpChanInfo; 
    DMX_OQ_Info_S  tmpOqInfo;
    HI_U32 u32AcqedNum = 0;
    HI_S32 s32Ret;

    IsDmxDevInit();
    pChanInfo = &pDmxDevOsi->DmxChanInfo[u32ChId];
    if (HI_UNF_DMX_CHAN_PLAY_EN != (HI_UNF_DMX_CHAN_PLAY_EN & pChanInfo->ChanStatus))
    {
        HI_WARN_DEMUX("channel %d is not open yet or not play channel\n", u32ChId);

        return HI_ERR_DMX_NOT_SUPPORT;
    }
    if ((HI_UNF_DMX_CHAN_TYPE_AUD == pChanInfo->ChanType) || (HI_UNF_DMX_CHAN_TYPE_VID == pChanInfo->ChanType))
    {
        return HI_ERR_DMX_NOT_SUPPORT;   // do not receive vid and aud channel
    }
    if (!DMX_OsiGetChnDataFlag(u32ChId))//channel have no data
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }
    if (HI_UNF_DMX_CHAN_TYPE_PES == pChanInfo->ChanType)
    {        
        HI_U32 u32CurDropCnt;
        u32CurDropCnt = s_u32DMXDropCnt[pChanInfo->ChanId];
        memcpy(&stTmpChanInfo,pChanInfo,sizeof(DMX_ChanInfo_S));
        pChanInfo->u32ChnResetLock = 1;
        s32Ret = DMXOsiFindPes(&stTmpChanInfo,psMsgList,16, &u32AcqedNum);
        if (u32CurDropCnt != s_u32DMXDropCnt[pChanInfo->ChanId])//drop occurs
        {
            pChanInfo->u32PesBlkCnt   = 0;
            pChanInfo->u32PesLength   = 0;
            pChanInfo->u32ProcsOffset = 0;
        }
        pChanInfo->u32ChnResetLock = 0;
    }
    else
    {
        memcpy(&stTmpChanInfo,pChanInfo,sizeof(DMX_ChanInfo_S));
        memcpy(&tmpOqInfo,&g_pDmxDevOsi->DmxOqInfo[pChanInfo->ChanOqId],sizeof(DMX_OQ_Info_S));
        pChanInfo->u32ChnResetLock = 1;
        s32Ret = DMXOsiReadSec(&stTmpChanInfo,1,&u32AcqedNum,psMsgList,&tmpOqInfo,0);
        pChanInfo->u32ChnResetLock = 0;
    }

    if (HI_SUCCESS == s32Ret && u32AcqedNum)
    {
        return HI_SUCCESS;
    }
    return HI_ERR_DMX_NOAVAILABLE_DATA;
}
HI_S32 DMX_OsiReadEsRequset(HI_U32 ChanId, DMX_Stream_S *EsData)
{
    HI_S32              ret;
    DMX_DEV_OSI_S      *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S     *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_Sub_DevInfo_S  *DmxInfo     = HI_NULL;
    DMX_FQ_Info_S      *FqInfo      = HI_NULL;
    DMX_OQ_Info_S      *OqInfo      = HI_NULL;
    HI_U32              OqRead      = 0;
    HI_U32              OqWrite     = 0;
    HI_U32              PvrCtrl     = 0;
    HI_U32              DataLen     = 0;
    HI_U32              DataPhyAddr = 0;
    HI_U32              DataKerAddr = 0;
    HI_U32              StartPhyAddr= 0;
    HI_U32              PesHeadLen  = 0;
    HI_U32              tm;
    Disp_Control_t      stDispController;

    CHECKDMXID(ChanInfo->DmxId);
    
    if ((HI_UNF_DMX_CHAN_TYPE_VID != ChanInfo->ChanType) && (HI_UNF_DMX_CHAN_TYPE_AUD != ChanInfo->ChanType))
    {
        HI_WARN_DEMUX("channel %d type is not av\n", ChanId);

        return HI_ERR_DMX_NOT_SUPPORT;
    }

    if (HI_UNF_DMX_CHAN_PLAY_EN != (HI_UNF_DMX_CHAN_PLAY_EN & ChanInfo->ChanStatus))
    {
        HI_WARN_DEMUX("channel %d is not open yet or not play channel\n", ChanId);

        return HI_ERR_DMX_NOT_OPEN_CHAN;
    }

    OqInfo  = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
    FqInfo  = &pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId];
    DmxInfo = &pDmxDevOsi->SubDevInfo[ChanInfo->DmxId];

    ChanInfo->u32TotolAcq++;

    HI_DRV_SYS_GetTimeStampMs(&tm);

    ChanInfo->u32AcqTimeInterval    = tm - ChanInfo->u32AcqTime;
    ChanInfo->u32AcqTime            = tm;

    OqRead = OqInfo->u32ProcsBlk;

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, &OqWrite, HI_NULL);

    if (OqRead == OqWrite)
    {
        if (!ChanInfo->ChanEosFlag)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        ret = DMXOsiOQGetBbsAddrSize(OqInfo->u32OQId, OqWrite, &StartPhyAddr, &DataLen, &PvrCtrl);
        if (HI_SUCCESS == ret)
        {
            DMX_RamPort_Info_S *PortInfo = &pDmxDevOsi->RamPortInfo[DmxInfo->PortId];
            HI_U32              size;

            DataPhyAddr = StartPhyAddr + OqInfo->u32ProcsOffset;
            DataKerAddr = FqInfo->u32BufVirAddr + (DataPhyAddr - FqInfo->u32BufPhyAddr);
            DataLen     -= OqInfo->u32ProcsOffset;

            if (0 == DataLen)
            {
                size = GetQueueLenth(PortInfo->Read, PortInfo->Write, PortInfo->BufSize);
                if (size > DMX_TS_BUFFER_EMPTY)
                {
                    return HI_ERR_DMX_NOAVAILABLE_DATA;
                }

                ChanInfo->ChanEosFlag = HI_FALSE;

                return HI_ERR_DMX_EMPTY_BUFFER;
            }

            if (0 != OqInfo->u32ProcsOffset)
            {
                PvrCtrl &= ~DMX_MASK_BIT_2; // clear sop flag
            }

            if (PvrCtrl & DMX_MASK_BIT_2)   // sop flag == 1
            {
                ret = DmxGetPesHeaderLen((HI_U8*)DataKerAddr, DataLen, &PesHeadLen);
                if ((HI_FAILURE == ret) || ((HI_SUCCESS == ret) && (DataLen <= PesHeadLen)))
                {
                    size = GetQueueLenth(PortInfo->Read, PortInfo->Write, PortInfo->BufSize);
                    if (size > DMX_TS_BUFFER_EMPTY)
                    {
                        return HI_ERR_DMX_NOAVAILABLE_DATA;
                    }

                    ChanInfo->ChanEosFlag = HI_FALSE;

                    return HI_ERR_DMX_EMPTY_BUFFER;
                }
            }

            DMXOsiOQGetReadWrite(OqInfo->u32OQId, &OqWrite, HI_NULL);

            if ((OqRead == OqWrite) && (0 == DataLen))
            {
                return HI_ERR_DMX_NOAVAILABLE_DATA;
            }
        }
        else
        {
            DMXOsiOQGetReadWrite(OqInfo->u32OQId, &OqWrite, HI_NULL);//flush OqWrite again
            if (OqRead == OqWrite)
            {
                /*for  DTS2014012901647 , we add this 'if' */
                if (ChanInfo->ChanEosFlag)
                {
                    ChanInfo->ChanEosFlag = HI_FALSE;
                    return HI_ERR_DMX_EMPTY_BUFFER;
                }
                return HI_ERR_DMX_NOAVAILABLE_DATA;
            }
        }
    }

    if (OqRead != OqWrite)
    {
        OQ_DescInfo_S  *OqDesc      = (OQ_DescInfo_S*)(OqInfo->u32OQVirAddr + OqRead * DMX_OQ_DESC_SIZE);
        HI_U32          CurrOffset  = OqInfo->u32ProcsOffset;

        StartPhyAddr = OqDesc->start_addr;

        DataPhyAddr = StartPhyAddr;
        DataLen     = (OqDesc->pvrctrl_datalen & 0xffff);
        PvrCtrl     = (OqDesc->pvrctrl_datalen >> 16) & 0xff;
        
        BUG_ON(!(DataPhyAddr >= FqInfo->u32BufPhyAddr && DataPhyAddr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize));

        DMXINC(OqInfo->u32ProcsBlk, OqInfo->u32OQDepth);

        OqInfo->u32ProcsOffset = 0;

        if (DataLen == CurrOffset)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;;
        }

        DataPhyAddr += CurrOffset;
        DataLen     -= CurrOffset;

        if (0 != CurrOffset)
        {
            PvrCtrl &= ~DMX_MASK_BIT_2; // clear sop flag
        }
    }

    DataKerAddr = FqInfo->u32BufVirAddr + (DataPhyAddr - FqInfo->u32BufPhyAddr);
    //add by yangchun: defaut value for there is no disp time;
    EsData->u32DispTime = ChanInfo->stLastControl.u32DispTime;
    EsData->u32DispEnableFlag= ChanInfo->stLastControl.u32DispEnableFlag;
    EsData->u32DispFrameDistance= ChanInfo->stLastControl.u32DispFrameDistance;
    EsData->u32DistanceBeforeFirstFrame= ChanInfo->stLastControl.u32DistanceBeforeFirstFrame;
    EsData->u32GopNum = ChanInfo->stLastControl.u32GopNum;

    if (PvrCtrl & DMX_MASK_BIT_2)   // sop flag == 1
    {
        ChanInfo->u32PesLength = 0;

        ret = ParsePesHeader(ChanInfo, DataKerAddr, DataLen, &PesHeadLen, &stDispController);
        if (HI_SUCCESS == ret)
        {
            HI_U8 *buf = (HI_U8*)DataKerAddr;

            ChanInfo->u32PesLength = ((buf[4] << 8) | buf[5]) - PesHeadLen + 6;

            DataLen     -= PesHeadLen;
            DataPhyAddr += PesHeadLen;
            DataKerAddr += PesHeadLen;
        }
        else if (-2 == ret)
        {
            if (DataLen >= PesHeadLen)
            {
                DataPhyAddr += PesHeadLen;
                DataKerAddr += PesHeadLen;
                DataLen     -= PesHeadLen;
                EsData->u32DispTime = stDispController.u32DispTime;
                EsData->u32DispEnableFlag= stDispController.u32DispEnableFlag;
                EsData->u32DispFrameDistance= stDispController.u32DispFrameDistance;
                EsData->u32DistanceBeforeFirstFrame= stDispController.u32DistanceBeforeFirstFrame;
                EsData->u32GopNum = stDispController.u32GopNum;
                memcpy(&(ChanInfo->stLastControl), &stDispController, sizeof(Disp_Control_t));
            }
            else
            {
                HI_ERR_DEMUX("DMX Unexpect error buflen=%#x, peshead=%#x\n", DataLen, PesHeadLen);
            }
        }
    }

    if (OqRead == OqWrite)
    {
        OqInfo->u32ProcsOffset = DataPhyAddr + DataLen - StartPhyAddr;
    }

    if (ChanInfo->ChanType == HI_UNF_DMX_CHAN_TYPE_AUD && ChanInfo->u32PesLength)
    {
        // Attention: for audio channel, del the unuse data, the length may error
        if (ChanInfo->u32PesLength < DataLen)
        {
            DataLen = ChanInfo->u32PesLength;

            ChanInfo->u32PesLength = 0;
            s_u32DMXPesLenErr[ChanInfo->ChanId]++;
        }
        else
        {
            ChanInfo->u32PesLength -= DataLen;
        }
    }

    EsData->u32BufPhyAddr   = DataPhyAddr;
    EsData->u32BufVirAddr   = DataKerAddr;
    EsData->u32BufLen       = DataLen;
    EsData->u32PtsMs        = ChanInfo->LastPts;
    ChanInfo->LastPts = INVALID_PTS;

#ifdef HI_DEMUX_PROC_SUPPORT
    DMX_OsrSaveEs(ChanInfo->ChanType, (HI_U8*)EsData->u32BufVirAddr, EsData->u32BufLen,ChanId);
#endif

    ChanInfo->u32HitAcq++;

    return HI_SUCCESS;
}

HI_S32 DMX_OsiReleaseReadEs(HI_U32 ChanId, DMX_Stream_S *EsData)
{
    HI_S32          ret;
    DMX_DEV_OSI_S  *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    DMX_FQ_Info_S  *FqInfo;
    DMX_OQ_Info_S  *OqInfo;
    HI_U32          TimeMs;

    CHECKDMXID(ChanInfo->DmxId);

    if ((HI_UNF_DMX_CHAN_TYPE_VID != ChanInfo->ChanType) && (HI_UNF_DMX_CHAN_TYPE_AUD != ChanInfo->ChanType))
    {
        HI_ERR_DEMUX("channel %d type is not av\n", ChanId);

        return HI_ERR_DMX_NOT_SUPPORT;
    }

    if (HI_UNF_DMX_CHAN_PLAY_EN != (HI_UNF_DMX_CHAN_PLAY_EN & ChanInfo->ChanStatus))
    {
        HI_ERR_DEMUX("channel %d is not open yet or not play channel\n", ChanId);

        return HI_ERR_DMX_NOT_OPEN_CHAN;
    }

    OqInfo = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
    FqInfo = &pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId];

    if (!(EsData->u32BufPhyAddr >= FqInfo->u32BufPhyAddr && EsData->u32BufPhyAddr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize))
    {
        HI_ERR_DEMUX("Release es data range[0x%x, 0x%x] is out of buff range[0x%x, 0x%x].\n", EsData->u32BufPhyAddr, EsData->u32BufPhyAddr + EsData->u32BufLen,
                                            FqInfo->u32BufPhyAddr, FqInfo->u32BufPhyAddr + FqInfo->u32BufSize);

        return HI_ERR_DMX_INVALID_PARA;
    }

    ChanInfo->u32Release++;

    HI_DRV_SYS_GetTimeStampMs(&TimeMs);

    ChanInfo->u32RelTimeInterval = TimeMs - ChanInfo->u32RelTime;
    ChanInfo->u32RelTime = TimeMs;

    do
    {
        OQ_DescInfo_S  *OqDesc;
        HI_U32          OqRead;
        HI_U32          OqWrite;
        HI_U32          DataPhyAddr;
        HI_U32          DataLen;
        HI_U32          Size;

        DMXOsiOQGetReadWrite(OqInfo->u32OQId, &OqWrite, &OqRead);

        if (OqRead == OqWrite)
        {
            return HI_SUCCESS;
        }

        OqDesc = (OQ_DescInfo_S*)(OqInfo->u32OQVirAddr + OqRead * DMX_OQ_DESC_SIZE);

        DataPhyAddr = OqDesc->start_addr;
        DataLen     = OqDesc->pvrctrl_datalen & 0xffff;

        BUG_ON(!(DataPhyAddr >= FqInfo->u32BufPhyAddr && DataPhyAddr < FqInfo->u32BufPhyAddr + FqInfo->u32BufSize));

        if ((EsData->u32BufPhyAddr < DataPhyAddr) || ((EsData->u32BufPhyAddr + EsData->u32BufLen) > (DataPhyAddr + DataLen)))
        {
            ret = DmxOQRelease(OqInfo->u32FQId, ChanInfo->ChanOqId, DataPhyAddr, DataLen);
            if (HI_SUCCESS != ret)
            {
                return ret;
            }

            DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[ChanInfo->DmxId], OqInfo->u32FQId);

            continue;
        }

        if ((EsData->u32BufPhyAddr < DataPhyAddr) || ((EsData->u32BufPhyAddr + EsData->u32BufLen) > (DataPhyAddr + DataLen)))
        {
            HI_ERR_DEMUX("channel %d release error, Rel: Addr=0x%x, len=0x%x, Oq: Addr=0x%x, len=0x%x\n",
                ChanId, EsData->u32BufPhyAddr, EsData->u32BufLen, DataPhyAddr, DataLen);

            return HI_ERR_DMX_INVALID_PARA;
        }

        Size = (EsData->u32BufPhyAddr + EsData->u32BufLen) - DataPhyAddr;
        if (Size < DataLen)
        {
            return HI_SUCCESS;
        }

        ret = DmxOQRelease(OqInfo->u32FQId, ChanInfo->ChanOqId, DataPhyAddr, DataLen);
        if (HI_SUCCESS == ret)
        {
            DMXCheckFQBPStatus(&pDmxDevOsi->SubDevInfo[ChanInfo->DmxId], OqInfo->u32FQId);
        }
    } while (0);

    return HI_SUCCESS;
}
#ifdef HI_DMX_PES_EXT_DATA_SUPPORT
HI_S32 DRV_DMX_GetPesExtData(HI_U32 ChanId, HI_MPI_PES_EXT_DATA_S *pstPesExtData)
{
    DMX_DEV_OSI_S      *pDmxDevOsi  = g_pDmxDevOsi;
    DMX_ChanInfo_S     *ChanInfo    = &pDmxDevOsi->DmxChanInfo[ChanId];
    
    CHECKDMXID(ChanInfo->DmxId);

    mutex_lock(&(ChanInfo->stDrvPesExtData.pes_lock));
    if (ChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType & HI_MPI_DMX_PES_EXT1_DATA_TYPE)
    {
        pstPesExtData->ePesExtDataType = HI_MPI_DMX_PES_EXT1_DATA_TYPE;
        pstPesExtData->u16PesExt1DataLen = ChanInfo->stDrvPesExtData.stPesExtData.u16PesExt1DataLen;
        if (pstPesExtData->u16PesExt1DataLen <= PES_EXT1_DATA_MAXSIZE)
        {
            memcpy(pstPesExtData->au8PesExt1Data, ChanInfo->stDrvPesExtData.stPesExtData.au8PesExt1Data, 
                pstPesExtData->u16PesExt1DataLen);
        }
        else
        {
            HI_ERR_DEMUX("The private data1 overflow!");
            mutex_unlock(&(ChanInfo->stDrvPesExtData.pes_lock));
            return HI_FAILURE;
        }
    }

    if (ChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType & HI_MPI_DMX_PES_EXT2_DATA_TYPE)
    {
        pstPesExtData->ePesExtDataType |= HI_MPI_DMX_PES_EXT2_DATA_TYPE;
        pstPesExtData->u16PesExt1DataLen = ChanInfo->stDrvPesExtData.stPesExtData.u16PesExt1DataLen;
        if (pstPesExtData->u16PesExt2DataLen <= PES_EXT2_DATA_MAXSIZE)
        {
            memcpy(pstPesExtData->au8PesExt2Data, ChanInfo->stDrvPesExtData.stPesExtData.au8PesExt2Data, 
                pstPesExtData->u16PesExt2DataLen);
        }
        else
        {
            HI_ERR_DEMUX("The private data2 overflow!");
            mutex_unlock(&(ChanInfo->stDrvPesExtData.pes_lock));
            return HI_FAILURE;
        }
    }
    ChanInfo->stDrvPesExtData.stPesExtData.ePesExtDataType = HI_MPI_DMX_PES_EXT_DATA_TYPE_BUTT;
    mutex_unlock(&(ChanInfo->stDrvPesExtData.pes_lock));
    

    return HI_SUCCESS;
}
#endif

HI_S32 DMX_OsiGetFQBufStatus(HI_U32 FQId, HI_MPI_DMX_BUF_STATUS_S *pBufStat)
{
    HI_U32 Read, Write;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[FQId];

    Read    = DmxHalGetFQReadPtr(FQId);
    Write   = DmxHalGetFQWritePtr(FQId);
	
    pBufStat->u32BufSize   = FqInfo->u32BufSize;
    pBufStat->u32UsedSize  = 0;
    pBufStat->u32BufRptr    = Read;
    pBufStat->u32BufWptr    = Write;
    
    if (Read < Write)
    {
        pBufStat->u32UsedSize = (FqInfo->u32FQDepth - 1 - Write + Read) * FqInfo->u32BlockSize;
    }
    else if (Read > Write)
    {
        pBufStat->u32UsedSize = (Read - Write) * FqInfo->u32BlockSize;
    }

    return HI_SUCCESS;
}

HI_S32 DMX_OsiGetChanBufStatus(HI_U32 ChanId, HI_MPI_DMX_BUF_STATUS_S *BufStatus)
{
    DMX_DEV_OSI_S  *pDmxDevOsi = g_pDmxDevOsi;
    DMX_ChanInfo_S *ChanInfo;
    DMX_OQ_Info_S  *OqInfo;

    IsDmxDevInit();

    ChanInfo = &pDmxDevOsi->DmxChanInfo[ChanId];

    CHECKDMXID(ChanInfo->DmxId);

    OqInfo = &pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
    if (OqInfo->enOQBufMode == DMX_OQ_MODE_UNUSED)
    {
        HI_ERR_DEMUX("channel %d OQ status not used.\n", ChanId);
        return HI_ERR_DMX_INVALID_PARA;
    }

    return DMX_OsiGetFQBufStatus(OqInfo->u32FQId, BufStatus);
}

HI_S32 DMX_OsiSelectDataFlag(HI_U32 *pu32WatchCh, HI_U32 u32WatchNum, HI_U32 *pu32Flag, HI_U32 u32TimeOutMs)
{
    HI_S32 ret;
    HI_U32 i;
    HI_U32 u32OqId;
    HI_U32 u32ChanId = 0;
    HI_U32 *pu32ChGroup = HI_NULL;
    DMX_DEV_OSI_S *pDmxDevOsi = g_pDmxDevOsi;
    wait_queue_head_t GroupWaitQueue;
    wait_queue_head_t* pWaitQueue;
    HI_SIZE_T u32LockFlag;

    /* mark enabled OQ EOP interrupt and finally disable it */
    HI_U32 u32EnOQIntMark[DMX_OQ_CNT] = {0}; 

    IsDmxDevInit();

    init_waitqueue_head(&GroupWaitQueue);
    s_pAllChnWatchWaitQueue = HI_NULL;
    if (HI_NULL == pu32WatchCh)
    {
        pu32ChGroup = s_u32DMXChanDatafl;
        u32WatchNum = DMX_CHANNEL_CNT;
        for (i = 0; i < DMX_CHANNEL_CNT; i++)
        {
            pu32ChGroup[i] = i;
        }

        spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
        s_pAllChnWatchWaitQueue = &AllChnWaitQueue;
        spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
        pWaitQueue = &AllChnWaitQueue;
    }
    else
    {
        pu32ChGroup = pu32WatchCh;
        pWaitQueue = &GroupWaitQueue;
    }

    pu32Flag[0] = 0;
    pu32Flag[1] = 0;
    pu32Flag[2] = 0;
    DMXOsiGetDataFlag(pu32ChGroup, u32WatchNum, pu32Flag);
    if (pu32Flag[0] || pu32Flag[1] || pu32Flag[2])
    {
        return HI_SUCCESS;
    }

    if (u32TimeOutMs)
    {
        s_u32DMXBufData = 0;
        for (i = 0; i < u32WatchNum; i++)
        {
            u32ChanId = pu32ChGroup[i];
            
            BUG_ON(u32ChanId >= DMX_CHANNEL_CNT);

            if ( (DMX_CNT <= pDmxDevOsi->DmxChanInfo[u32ChanId].DmxId)
                || (HI_UNF_DMX_CHAN_TYPE_AUD == pDmxDevOsi->DmxChanInfo[u32ChanId].ChanType)
                || (HI_UNF_DMX_CHAN_TYPE_VID == pDmxDevOsi->DmxChanInfo[u32ChanId].ChanType) 
                || (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY != (pDmxDevOsi->DmxChanInfo[u32ChanId].ChanOutMode & HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY))  )
            {
                continue; 
            }

            if (!s_pAllChnWatchWaitQueue)
            {
                u32OqId = pDmxDevOsi->DmxChanInfo[u32ChanId].ChanOqId;
            }
            else
            {
                u32OqId = u32ChanId;
            }
            
            BUG_ON(u32OqId >= DMX_OQ_CNT);
            
            spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
            pDmxDevOsi->DmxOqInfo[u32OqId].pWatchWaitQueue = pWaitQueue;
            spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
            
            u32EnOQIntMark[u32OqId] ++;
            DmxHalEnableOQEopInt(u32OqId);
        }

        u32TimeOutMs = (u32TimeOutMs * HZ) / 1000;
        ret = wait_event_interruptible_timeout(*pWaitQueue,
                                               s_u32DMXBufData,
                                               u32TimeOutMs);
        for (i = 0; i < u32WatchNum; i++)
        {
            u32ChanId = pu32ChGroup[i];
            
            BUG_ON(u32ChanId >= DMX_CHANNEL_CNT);

            if (!s_pAllChnWatchWaitQueue)
            {
                u32OqId = pDmxDevOsi->DmxChanInfo[u32ChanId].ChanOqId;
            }
            else
            {
                u32OqId = u32ChanId;
            }

            BUG_ON(u32OqId >= DMX_OQ_CNT);

            if (0 != u32EnOQIntMark[u32OqId])
            {
                DmxHalDisableOQEopInt(u32OqId);
                u32EnOQIntMark[u32OqId] --;

                spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
                pDmxDevOsi->DmxOqInfo[u32OqId].pWatchWaitQueue = HI_NULL;
                spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
            }            
        }

        spin_lock_irqsave(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);
        s_pAllChnWatchWaitQueue = HI_NULL;
        spin_unlock_irqrestore(&(pDmxDevOsi->splock_OqBuf), u32LockFlag);

        DMXOsiGetDataFlag(pu32ChGroup, u32WatchNum, pu32Flag);
        if ((pu32Flag[0] | pu32Flag[1] | pu32Flag[2]))
        {
            return HI_SUCCESS;
        }

        if (0 == ret)
        {
            HI_WARN_DEMUX("wait time out!\n");
            return HI_ERR_DMX_TIMEOUT;
        }

        DMXOsiGetDataFlag(pu32ChGroup, u32WatchNum, pu32Flag);
        if ((pu32Flag[0] | pu32Flag[1] | pu32Flag[2]))
        {
            return HI_SUCCESS;
        }
    }

    return HI_ERR_DMX_NOAVAILABLE_DATA;
}

HI_S32 DMX_OsiPcrChannelCreate(const HI_U32 DmxId, HI_U32 *PcrId)
{
    HI_S32          ret         = HI_ERR_DMX_NOFREE_CHAN;
    DMX_DEV_OSI_S  *DmxDevOsi   = g_pDmxDevOsi;
    HI_U32          i;

    for (i = 0; i < DMX_PCR_CHANNEL_CNT; i++)
    {
        DMX_PCR_Info_S *PcrInfo = &DmxDevOsi->DmxPcrInfo[i];

        if (PcrInfo->DmxId >= DMX_CNT)
        {
            PcrInfo->DmxId      = DmxId;
            PcrInfo->PcrPid     = DMX_INVALID_PID;
            PcrInfo->SyncHandle = DMX_INVALID_SYNC_HANDLE;
            PcrInfo->PcrValue   = 0xffffffff;
            PcrInfo->ScrValue   = 0xffffffff;

            DmxHalSetPcrPid(i, DMX_INVALID_PID);

            *PcrId = i;

            ret = HI_SUCCESS;

            break;
        }
    }

    return ret;
}

HI_S32 DMX_OsiPcrChannelDestroy(const HI_U32 PcrId)
{
    HI_S32          ret     = HI_ERR_DMX_INVALID_PARA;
    DMX_PCR_Info_S *PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[PcrId];

    if (PcrInfo->DmxId < DMX_CNT)
    {
        PcrInfo->DmxId = DMX_CNT;

        DmxHalSetPcrIntEnable(PcrId, HI_FALSE);

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiPcrChannelSetPid(const HI_U32 PcrId, const HI_U32 PcrPid)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_PCR_Info_S *PcrInfo = &DmxMgr->DmxPcrInfo[PcrId];

    if ((PcrPid > DMX_INVALID_PID) || (PcrInfo->DmxId >= DMX_CNT))
    {
        HI_ERR_DEMUX("Invalid parameter: PcrPid=0x%x, DmxId=%u\n", PcrPid, PcrInfo->DmxId);

        return HI_ERR_DMX_INVALID_PARA;
    }

    if (PcrPid < DMX_INVALID_PID)
    {
        HI_U32 i;

        for (i = 0; i < DMX_PCR_CHANNEL_CNT; i++)
        {
            if ((DmxMgr->DmxPcrInfo[i].DmxId == PcrInfo->DmxId) && (i != PcrId))
            {
                if (DmxMgr->DmxPcrInfo[i].PcrPid == PcrPid)
                {
                    PcrInfo->PcrPid = PcrPid;
                    HI_ERR_DEMUX("chanel(%x) pcr pid(%x) already used.\n", PcrId, PcrPid);
                    return HI_SUCCESS;
                }
            }
        }
    }

    PcrInfo->PcrPid = PcrPid;
    DmxHalSetPcrPid(PcrId, PcrPid);
    DmxHalSetPcrDmxId(PcrId, PcrInfo->DmxId);
    DmxHalSetPcrIntEnable(PcrId, HI_TRUE);

    return HI_SUCCESS;
}

HI_S32 DMX_OsiPcrChannelGetPid(const HI_U32 PcrId, HI_U32 *PcrPid)
{
    HI_S32          ret     = HI_ERR_DMX_INVALID_PARA;
    DMX_PCR_Info_S *PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[PcrId];

    if (PcrInfo->DmxId < DMX_CNT)
    {
        *PcrPid = PcrInfo->PcrPid;

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiPcrChannelGetClock(const HI_U32 PcrId, HI_U64 *PcrValue, HI_U64 *ScrValue)
{
    HI_S32          ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S  *DmxDevOsi   = g_pDmxDevOsi;
    DMX_PCR_Info_S *PcrInfo     = &DmxDevOsi->DmxPcrInfo[PcrId];

    if (PcrInfo->DmxId < DMX_CNT)
    {
        *PcrValue = PcrInfo->PcrValue;
        *ScrValue = PcrInfo->ScrValue;

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiPcrChannelAttachSync(const HI_U32 PcrId, const HI_U32 SyncHadle)
{
    HI_S32          ret     = HI_ERR_DMX_INVALID_PARA;
    DMX_PCR_Info_S *PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[PcrId];

    if (PcrInfo->DmxId < DMX_CNT)
    {
        PcrInfo->SyncHandle = SyncHadle;

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiPcrChannelDetachSync(const HI_U32 PcrId)
{
    HI_S32          ret     = HI_ERR_DMX_INVALID_PARA;
    DMX_PCR_Info_S *PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[PcrId];

    if (PcrInfo->DmxId < DMX_CNT)
    {
        PcrInfo->SyncHandle = DMX_INVALID_SYNC_HANDLE;

        ret = HI_SUCCESS;
    }

    return ret;
}

#ifdef HI_DEMUX_REC_SUPPORT
HI_S32 DMX_DRV_REC_CreateChannel(HI_UNF_DMX_REC_ATTR_S *RecAttr, DMX_REC_TIMESTAMP_MODE_E enRecTimeStamp,HI_U32 *RecId, HI_U32 *BufPhyAddr, HI_U32 *BufSize)
{
    HI_S32          ret             = HI_ERR_DMX_NOFREE_CHAN;
    DMX_DEV_OSI_S  *DmxMgr          = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo         = HI_NULL;
    DMX_FQ_Info_S  *RecFqInfo       = HI_NULL;
    DMX_OQ_Info_S  *RecOqInfo       = HI_NULL;
	HI_U32          RecFqId         = DMX_INVALID_FQ_ID;
    HI_U32          RecBufSize      = 0;
    HI_U32          RecFqBufSize    = 0;
    HI_U32          RecFqBlockSize  = DMX_REC_VID_TS_PACKETS_PER_BLOCK;
    HI_U32          RecFqDepth      = 0;
    HI_U32          RecOqBufSize    = 0;
    HI_U32          RecOqDepth      = 0;
	HI_U32          Size            = 0;
    HI_S32          PicParser       = -1;
    VIDSTD_E        VidStd          = VIDSTD_BUTT;
    MMZ_BUFFER_S    RecMmzBuf       = {0};
    HI_CHAR         BufName[16];
    HI_U32          ScdBufSize      = 0;
    HI_U32          ScdBlockSize    = 0;
#ifdef HI_DEMUX_SCD_SUPPORT
    HI_U32          ScdFqBufSize    = 0;
    HI_U32          ScdFqDepth      = 0;
    HI_U32          ScdOqBufSize    = 0;
    HI_U32          ScdOqDepth      = 0;

	HI_U32          ScdFqId         = DMX_INVALID_FQ_ID;
	DMX_FQ_Info_S  *ScdFqInfo       = HI_NULL;
	MMZ_BUFFER_S    ScdMmzBuf       = {0};
	DMX_OQ_Info_S  *ScdOqInfo       = HI_NULL;
#endif

    CHECKDMXID(RecAttr->u32DmxId);

#ifndef DMX_REC_TIME_STAMP_SUPPORT
    enRecTimeStamp = DMX_REC_TIMESTAMP_NONE;
#endif

    switch (RecAttr->enRecType)
    {
        case HI_UNF_DMX_REC_TYPE_SELECT_PID :
            switch (RecAttr->enIndexType)
            {
                case HI_UNF_DMX_REC_INDEX_TYPE_VIDEO :
                    if (RecAttr->u32IndexSrcPid >= DMX_INVALID_PID)
                    {
                        HI_ERR_DEMUX("invalid index pid:0x%x\n",RecAttr->u32IndexSrcPid);
                        return HI_ERR_DMX_INVALID_PARA;
                    }

                    switch (RecAttr->enVCodecType)
                    {
                        case HI_UNF_VCODEC_TYPE_MPEG2 :
                            VidStd = VIDSTD_MPEG2;
                            break;

                        case HI_UNF_VCODEC_TYPE_MPEG4 :
                            VidStd = VIDSTD_MPEG4;
                            break;

                        case HI_UNF_VCODEC_TYPE_AVS :
                            VidStd = VIDSTD_AVS;
                            break;

                        case HI_UNF_VCODEC_TYPE_H264 :
                            VidStd = VIDSTD_H264;
                            break;

                        default :
                            HI_ERR_DEMUX("invalid vcodec type:%d\n",RecAttr->enVCodecType);
                            return HI_ERR_DMX_INVALID_PARA;
                    }

                    ScdBufSize = DMX_REC_VID_SCD_BUF_SIZE;
                    ScdBlockSize = DMX_REC_VID_SCD_NUM_PER_BLOCK;

                    if ( enRecTimeStamp >= DMX_REC_TIMESTAMP_ZERO)
                    {
                        RecFqBlockSize  = DMX_REC_VID_TS_WITH_TIMESTAMP_BLOCK_SIZE;
                    }
                    break;

                case HI_UNF_DMX_REC_INDEX_TYPE_AUDIO :
                    if (RecAttr->u32IndexSrcPid >= DMX_INVALID_PID)
                    {
                        HI_ERR_DEMUX("invalid index pid:0x%x\n",RecAttr->u32IndexSrcPid);
                        return HI_ERR_DMX_INVALID_PARA;
                    }

                    ScdBufSize      = DMX_REC_AUD_SCD_BUF_SIZE;
                    ScdBlockSize = DMX_REC_AUD_SCD_NUM_PER_BLOCK;

                    if ( enRecTimeStamp >= DMX_REC_TIMESTAMP_ZERO)
                    {
                        RecFqBlockSize  = DMX_REC_AUD_TS_WITH_TIMESTAMP_BLOCK_SIZE;
                    }
                    else
                    {
                        RecFqBlockSize  = DMX_REC_AUD_TS_PACKETS_PER_BLOCK;
                    }
                    break;

                case HI_UNF_DMX_REC_INDEX_TYPE_NONE :
                    break;

                default :
                    return HI_ERR_DMX_INVALID_PARA;
            }

            break;

        case HI_UNF_DMX_REC_TYPE_ALL_PID :
            break;

        default :
            HI_ERR_DEMUX("invalid RecAttr->enRecType:%d\n",RecAttr->enRecType);
            return HI_ERR_DMX_INVALID_PARA;
    }

    if (RecAttr->u32RecBufSize < DMX_REC_MIN_BUF_SIZE)
    {
        HI_ERR_DEMUX("invalid RecAttr->u32RecBufSize:0x%x\n",RecAttr->u32RecBufSize);
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (0 == mutex_lock_interruptible(&DmxMgr->DmxRecInfo[RecAttr->u32DmxId].LockRec))
    {
        if (DmxMgr->DmxRecInfo[RecAttr->u32DmxId].DmxId >= DMX_CNT)
        {
            RecInfo = &DmxMgr->DmxRecInfo[RecAttr->u32DmxId];

            RecInfo->DmxId = RecAttr->u32DmxId;
        }
        mutex_unlock(&DmxMgr->DmxRecInfo[RecAttr->u32DmxId].LockRec);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        return HI_ERR_DMX_BUSY;
    }

    if (!RecInfo)
    {
        HI_ERR_DEMUX("no free rec channel or channel already in use!\n");
        return HI_ERR_DMX_NOFREE_CHAN;
    }

    RecFqId = DmxFqIdAcquire(1, DMX_FQ_CNT);
    if (RecFqId >= DMX_FQ_CNT)
    {
        HI_ERR_DEMUX("invalid fq id:%d!\n",RecFqId);
        goto exit;
    }

    RecFqInfo = &DmxMgr->DmxFqInfo[RecFqId];

    if (0 != ScdBufSize)
    {
        if (HI_UNF_DMX_REC_INDEX_TYPE_VIDEO == RecAttr->enIndexType)
        {
            PicParser = FIDX_OpenInstance(VidStd, STRM_TYPE_ES, (HI_U32*)&RecInfo->LastFrameInfo);
            if (PicParser < 0)
            {
                HI_ERR_DEMUX("PicParser is invlid\n");

                goto exit;
            }
        }
#ifdef HI_DEMUX_SCD_SUPPORT
        ScdFqId = DmxFqIdAcquire(1, DMX_FQ_CNT);
        if (ScdFqId >= DMX_FQ_CNT)
        {
            HI_ERR_DEMUX("invalid fq id:%d!\n",RecFqId);
            goto exit;
        }
#endif
    }

    RecBufSize      = (RecAttr->u32RecBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;
    /* align with block */
    RecBufSize = RecBufSize - RecBufSize % RecFqBlockSize;
    
    RecFqDepth      = RecBufSize / RecFqBlockSize + 1;
    RecFqBufSize    = RecFqDepth * DMX_FQ_DESC_SIZE;
    RecFqBufSize    = (RecFqBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;

    RecOqDepth      = RecFqDepth < DMX_OQ_DEPTH ? (RecFqDepth - 1) : DMX_OQ_DEPTH;
    RecOqBufSize    = RecOqDepth * DMX_OQ_DESC_SIZE;

    snprintf(BufName, 16,"DMX_RecBuf[%u]", RecFqId);

    Size = RecBufSize + RecFqBufSize + RecOqBufSize;

    if (HI_SUCCESS != HI_DRV_MMZ_AllocAndMap(BufName, MMZ_OTHERS, Size, 0, &RecMmzBuf))
    {
        HI_ERR_DEMUX("rec memory allocate failed, BufSize=0x%x\n", Size);

        ret = HI_ERR_DMX_ALLOC_MEM_FAILED;

        goto exit;
    }
    
#ifdef HI_DEMUX_SCD_SUPPORT
    if (0 != ScdBufSize)
    {
        ScdBufSize      = (ScdBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;
        /* align with block */
        ScdBufSize = ScdBufSize - ScdBufSize % ScdBlockSize;
        
        ScdFqDepth      = ScdBufSize / ScdBlockSize + 1;
        ScdFqBufSize    = ScdFqDepth * DMX_FQ_DESC_SIZE;
        ScdFqBufSize    = (ScdFqBufSize + MIN_MMZ_BUF_SIZE - 1) / MIN_MMZ_BUF_SIZE * MIN_MMZ_BUF_SIZE;

        ScdOqDepth      = ScdFqDepth < DMX_OQ_DEPTH ? (ScdFqDepth - 1) : DMX_OQ_DEPTH;
        ScdOqBufSize    = ScdOqDepth * DMX_OQ_DESC_SIZE;

        snprintf(BufName, 16,"DMX_ScdBuf[%u]", ScdFqId);

        Size = ScdBufSize + ScdFqBufSize + ScdOqBufSize;

        if (HI_SUCCESS != HI_DRV_MMZ_AllocAndMap(BufName, MMZ_OTHERS, Size, 0, &ScdMmzBuf))
        {
            HI_ERR_DEMUX("scd memory allocate failed, BufSize=0x%x\n", Size);

            ret = HI_ERR_DMX_ALLOC_MEM_FAILED;

            goto exit;
        }
    }
#endif

    if (0 == mutex_lock_interruptible(&RecInfo->LockRec))
    {
        RecInfo->RecType        = RecAttr->enRecType;
        RecInfo->Descramed      = RecAttr->bDescramed;
        RecInfo->IndexType      = RecAttr->enIndexType;
        if (0 == ScdBufSize)
        {
            RecInfo->IndexType  = HI_UNF_DMX_REC_INDEX_TYPE_NONE;
        }
        RecInfo->VCodecType     = RecAttr->enVCodecType;
        RecInfo->IndexPid       = RecAttr->u32IndexSrcPid;
        RecInfo->RecStatus      = DMX_REC_STATUS_STOP;
        RecInfo->RecFqId        = RecFqId;
        RecInfo->RecOqId        = DMX_CHANNEL_CNT + RecInfo->DmxId * 2;
#ifdef HI_DEMUX_SCD_SUPPORT
        RecInfo->ScdFqId        = ScdFqId;
        RecInfo->ScdOqId        = DMX_CHANNEL_CNT + RecInfo->DmxId * 2 + 1;
        RecInfo->ScdId          = RecAttr->u32DmxId + 1;
#endif
        RecInfo->PicParser      = PicParser;

        mutex_unlock(&RecInfo->LockRec);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        ret = HI_ERR_DMX_BUSY;

        goto exit;
    }

    memset(&RecInfo->LastFrameInfo, 0, sizeof(HI_UNF_DMX_REC_INDEX_S));

    RecFqInfo->u32BufPhyAddr    = RecMmzBuf.u32StartPhyAddr;
    RecFqInfo->u32BufVirAddr    = RecMmzBuf.u32StartVirAddr;
    RecFqInfo->u32BufSize       = RecBufSize;

    RecFqInfo->u32FQPhyAddr     = RecFqInfo->u32BufPhyAddr + RecBufSize;
    RecFqInfo->u32FQVirAddr     = RecFqInfo->u32BufVirAddr + RecBufSize;
    RecFqInfo->u32FQDepth       = RecFqDepth;
    RecFqInfo->u32BlockSize     = RecFqBlockSize;

    RecOqInfo = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];

    RecOqInfo->u32OQPhyAddr     = RecFqInfo->u32FQPhyAddr + RecFqBufSize;
    RecOqInfo->u32OQVirAddr     = RecFqInfo->u32FQVirAddr + RecFqBufSize;
    RecOqInfo->u32OQDepth       = RecOqDepth;
    RecOqInfo->u32FQId          = RecFqId;
    RecOqInfo->u32AttachId      = RecOqInfo->u32OQId;
    RecOqInfo->enOQBufMode      = DMX_OQ_MODE_REC;
#ifdef HI_DEMUX_SCD_SUPPORT
    if (0 != ScdBufSize)
    {
        ScdFqInfo = &DmxMgr->DmxFqInfo[ScdFqId];
        ScdOqInfo = &DmxMgr->DmxOqInfo[RecInfo->ScdOqId];

        ScdFqInfo->u32BufPhyAddr    = ScdMmzBuf.u32StartPhyAddr;
        ScdFqInfo->u32BufVirAddr    = ScdMmzBuf.u32StartVirAddr;
        ScdFqInfo->u32BufSize       = ScdBufSize;

        ScdFqInfo->u32FQPhyAddr     = ScdFqInfo->u32BufPhyAddr + ScdBufSize;
        ScdFqInfo->u32FQVirAddr     = ScdFqInfo->u32BufVirAddr + ScdBufSize;
        ScdFqInfo->u32FQDepth       = ScdFqDepth;
        ScdFqInfo->u32BlockSize     = ScdBlockSize;

        ScdOqInfo->u32OQPhyAddr     = ScdFqInfo->u32FQPhyAddr + ScdFqBufSize;
        ScdOqInfo->u32OQVirAddr     = ScdFqInfo->u32FQVirAddr + ScdFqBufSize;
        ScdOqInfo->u32OQDepth       = ScdOqDepth;
        ScdOqInfo->u32FQId          = ScdFqId;
        ScdOqInfo->u32AttachId      = ScdOqInfo->u32OQId;
        ScdOqInfo->enOQBufMode      = DMX_OQ_MODE_SCD;
    }
#endif
    *RecId      = RecAttr->u32DmxId;
    *BufPhyAddr = RecFqInfo->u32BufPhyAddr;
    *BufSize    = RecFqInfo->u32BufSize;;

	/*set recorded ts packet len,added by l00188263 ,Hi3719 support 192 Byte ts packet length recording*/
#ifdef DMX_REC_TIME_STAMP_SUPPORT
	if (0 == mutex_lock_interruptible(&RecInfo->LockRec))
    {
        RecInfo->enRecTimeStamp   = enRecTimeStamp;
        mutex_unlock(&RecInfo->LockRec);
    }
    else
    {
        HI_ERR_DEMUX("mutex_lock_interruptible failed!\n");
        return HI_ERR_DMX_BUSY;
    }
	
	DmxHalConfigRecTsTimeStamp(RecAttr->u32DmxId, enRecTimeStamp);
#endif	
    return HI_SUCCESS;

exit :
#ifdef HI_DEMUX_SCD_SUPPORT
    if (ScdMmzBuf.u32StartPhyAddr)
    {
        HI_DRV_MMZ_UnmapAndRelease(&ScdMmzBuf);
    }
#endif
    if (RecMmzBuf.u32StartPhyAddr)
    {
        HI_DRV_MMZ_UnmapAndRelease(&RecMmzBuf);
    }

    if (PicParser >= 0)
    {
        FIDX_CloseInstance(PicParser);
    }

    DmxFqIdRelease(RecFqId);
#ifdef HI_DEMUX_SCD_SUPPORT
    DmxFqIdRelease(ScdFqId);
#endif
    if (RecInfo)
    {
        if (0 == mutex_lock_interruptible(&RecInfo->LockRec))
        {
            RecInfo->DmxId = DMX_INVALID_DEMUX_ID;
            mutex_unlock(&RecInfo->LockRec);
        }
    }

    return ret;
}

HI_S32 DMX_DRV_REC_DestroyChannel(HI_U32 RecId)
{
    HI_S32          ret         = HI_ERR_DMX_INVALID_PARA;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo     = &DmxMgr->DmxRecInfo[RecId];

    if (RecInfo->DmxId < DMX_CNT)
    {
        if (DMX_REC_STATUS_STOP == RecInfo->RecStatus)
        {
            DMX_FQ_Info_S  *RecFqInfo = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
            MMZ_BUFFER_S    MmzBuf;

            MmzBuf.u32StartPhyAddr  = RecFqInfo->u32BufPhyAddr;
            MmzBuf.u32StartVirAddr  = RecFqInfo->u32BufVirAddr;

            HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);

            DmxFqIdRelease(RecInfo->RecFqId);

            if (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType)
            {
#ifdef HI_DEMUX_SCD_SUPPORT
                DMX_FQ_Info_S  *ScdFqInfo = &DmxMgr->DmxFqInfo[RecInfo->ScdFqId];

                MmzBuf.u32StartPhyAddr  = ScdFqInfo->u32BufPhyAddr;
                MmzBuf.u32StartVirAddr  = ScdFqInfo->u32BufVirAddr;

                HI_DRV_MMZ_UnmapAndRelease(&MmzBuf);

                DmxFqIdRelease(RecInfo->ScdFqId);
#endif
                if (HI_UNF_DMX_REC_INDEX_TYPE_VIDEO == RecInfo->IndexType)
                {
                    FIDX_CloseInstance(RecInfo->PicParser);
                }
            }

            if (0 == mutex_lock_interruptible(&RecInfo->LockRec))
            {
                RecInfo->DmxId = DMX_INVALID_DEMUX_ID;
                mutex_unlock(&RecInfo->LockRec);
            }

            ret = HI_SUCCESS;
        }
        else
        {
            HI_ERR_DEMUX("stop the rec channel first!\n");
            ret = HI_ERR_DMX_STARTING_REC_CHAN;
        }
    }

    return ret;
}

HI_S32 DMX_DRV_REC_AddRecPid(HI_U32 RecId, HI_U32 Pid, HI_U32 *ChanId)
{
    HI_S32                  ret;
    DMX_DEV_OSI_S          *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S          *RecInfo     = &DmxMgr->DmxRecInfo[RecId];
    HI_UNF_DMX_CHAN_ATTR_S  ChanAttr;
    DMX_MMZ_BUF_S           ChanBuf;

    if (Pid > DMX_INVALID_PID)
    {
        HI_ERR_DEMUX("Invalid pid:0x%x\n", Pid);

        return HI_ERR_DMX_INVALID_PARA;
    }

    CHECKDMXID(RecInfo->DmxId);

    ChanAttr.u32BufSize    = 0;
    ChanAttr.enChannelType = HI_UNF_DMX_CHAN_TYPE_POST;
    ChanAttr.enCRCMode     = HI_UNF_DMX_CHAN_CRC_MODE_FORBID;
    ChanAttr.enOutputMode  = HI_UNF_DMX_CHAN_OUTPUT_MODE_REC;

    ret = DMX_OsiCreateChannel(RecInfo->DmxId, &ChanAttr, &ChanBuf, ChanId);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    ret = DMX_OsiSetChannelPid(*ChanId, Pid);
    if (HI_SUCCESS != ret)
    {
        DMX_OsiDestroyChannel(*ChanId);

        return ret;
    }

    ret = DMX_OsiOpenChannel(*ChanId);
    if (HI_SUCCESS != ret)
    {
        DMX_OsiDestroyChannel(*ChanId);

        return ret;
    }

    return HI_SUCCESS;
}

HI_S32 DMX_DRV_REC_DelRecPid(HI_U32 RecId, HI_U32 ChanId)
{
    HI_S32          ret;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo     = &DmxMgr->DmxRecInfo[RecId];
    DMX_ChanInfo_S *ChanInfo    = &DmxMgr->DmxChanInfo[ChanId];

    if ((RecInfo->DmxId >= DMX_CNT) || (ChanInfo->DmxId >= DMX_CNT))
    {
        HI_ERR_DEMUX("RecInfo->DmxId:%d or ChanInfo->DmxId:%d invalid\n", RecInfo->DmxId,ChanInfo->DmxId);
        return HI_ERR_DMX_INVALID_PARA;
    }

    if (RecInfo->DmxId != ChanInfo->DmxId)
    {
        HI_ERR_DEMUX("RecInfo->DmxId:%d or ChanInfo->DmxId:%d not equal!\n", RecInfo->DmxId,ChanInfo->DmxId);
        return HI_ERR_DMX_UNMATCH_CHAN;
    }

    ret = DMX_OsiCloseChannel(ChanId);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    ret = DMX_OsiDestroyChannel(ChanId);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    return HI_SUCCESS;
}

HI_S32 DMX_DRV_REC_DelAllRecPid(HI_U32 RecId)
{
    HI_S32          ret         = HI_SUCCESS;
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo     = &DmxMgr->DmxRecInfo[RecId];
    DMX_ChanInfo_S *ChanInfo;
    HI_U32          i;


    CHECKDMXID(RecInfo->DmxId);

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        ChanInfo = &DmxMgr->DmxChanInfo[i];

        if ((RecInfo->DmxId == ChanInfo->DmxId) && (HI_UNF_DMX_CHAN_REC_EN == ChanInfo->ChanStatus))
        {
            ret = DMX_OsiCloseChannel(i);
            if (HI_SUCCESS != ret)
            {
                break;
            }

            ret = DMX_OsiDestroyChannel(i);
            if (HI_SUCCESS != ret)
            {
                break;
            }
        }
    }

    return ret;
}

HI_S32 DMX_DRV_REC_GetTsCnt(HI_U32 RecId,HI_U32* TSCnt)
{
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo     = &DmxMgr->DmxRecInfo[RecId];


    if (RecInfo->DmxId >= DMX_CNT)
    {
        return HI_ERR_DMX_INVALID_PARA;
    }
    
    *TSCnt = DmxHalGetOqCounter(RecInfo->RecOqId);
    return HI_SUCCESS;
}
HI_S32 DMX_DRV_REC_AddExcludeRecPid(HI_U32 RecId, HI_U32 Pid)
{
#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    HI_U32 i;
    HI_U32 tmpRecDmxID,tmpPid;
#endif

    if (Pid > DMX_INVALID_PID)
    {
        HI_ERR_DEMUX("Invalid pid 0x%x\n", Pid);

        return HI_ERR_DMX_INVALID_PARA;
    }

#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    for ( i = 0 ; i < DMX_REC_EXCLUDE_PID_NUM ; i++ )
    {
        DmxHalGetAllRecExcludePid(i,&tmpRecDmxID,&tmpPid);
        if ((tmpRecDmxID - 1) == RecId && tmpPid == Pid)
        {
            return HI_SUCCESS;//already added the pid
        }
    }

    for ( i = 0 ; i < DMX_REC_EXCLUDE_PID_NUM ; i++ )
    {
        DmxHalGetAllRecExcludePid(i,&tmpRecDmxID,&tmpPid);
        if (tmpRecDmxID == 0)
        {
            DmxHalSetAllRecExcludePid(i,RecId+1,Pid);
            return HI_SUCCESS;
        }
    }
    HI_ERR_DEMUX("no available exclude pid resource: 0x%x\n", Pid);
    return HI_ERR_DMX_NOAVAILABLE_EXCLUDEPID;
#else
    return HI_ERR_DMX_NOT_SUPPORT;
#endif
}

HI_S32 DMX_DRV_REC_DelExcludeRecPid(HI_U32 RecId, HI_U32 Pid)
{
#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    HI_U32 i;
    HI_U32 tmpRecDmxID,tmpPid;
#endif
    if (Pid > DMX_INVALID_PID)
    {
        HI_ERR_DEMUX("Invalid pid 0x%x\n", Pid);

        return HI_ERR_DMX_INVALID_PARA;
    }

#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    for ( i = 0 ; i < DMX_REC_EXCLUDE_PID_NUM ; i++ )
    {
        DmxHalGetAllRecExcludePid(i,&tmpRecDmxID,&tmpPid);
        if ((tmpRecDmxID - 1) == RecId && tmpPid == Pid)
        {
            DmxHalSetAllRecExcludePid(i,0,DMX_INVALID_PID);
            return HI_SUCCESS;
        }
    }
    return HI_SUCCESS;
#else
    return HI_ERR_DMX_NOT_SUPPORT;
#endif
}

HI_S32 DMX_DRV_REC_DelAllExcludeRecPid(HI_U32 RecId)
{
#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    HI_U32 i;
    HI_U32 tmpRecDmxID,tmpPid;
#endif

#ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    for ( i = 0 ; i < DMX_REC_EXCLUDE_PID_NUM ; i++ )
    {
        DmxHalGetAllRecExcludePid(i,&tmpRecDmxID,&tmpPid);
        if ((tmpRecDmxID - 1) == RecId)
        {
            DmxHalSetAllRecExcludePid(i,0,DMX_INVALID_PID);
        }
    }
    return HI_SUCCESS;
#else
    return HI_ERR_DMX_NOT_SUPPORT;
#endif
}

HI_S32 DMX_DRV_REC_StartRecChn(HI_U32 RecId)
{
    HI_U32          i;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];

    CHECKDMXID(RecInfo->DmxId);
    CHECKPOINTER(FqInfo);

    if (DMX_REC_STATUS_START == RecInfo->RecStatus)
    {
        return HI_SUCCESS;
    }
	
    BUG_ON(!list_empty(&RecInfo->head));

    /* Init the RecInfo->u64TsOffset as 0  when start */
    RecInfo->u32LastWrite = 0;
    RecInfo->u64TsOffset = 0;

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &DmxMgr->DmxChanInfo[i];

        if (ChanInfo->DmxId == RecInfo->DmxId)
        {
            if (HI_UNF_DMX_CHAN_OUTPUT_MODE_REC & ChanInfo->ChanOutMode)
            {
                DmxHalSetChannelRecBufId(ChanInfo->ChanId, RecInfo->RecOqId);

                if ((ChanInfo->ChanPid == RecInfo->IndexPid) && (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType))
                {
                    /* get rid of POST mode from channel whatever */
                    DmxHalSetChannelTsPostMode(ChanInfo->ChanId, DMX_DISABLE);
                    
                    DmxHalSetSCDAttachChannel(RecInfo->ScdId, ChanInfo->ChanId);
                }
            }
        }
    }
    
#ifdef HI_DEMUX_SCD_SUPPORT //belongs to HI_DEMUX_REC_SUPPORT
    if (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType)
    {
        DmxHalAllocSCDBufferId(RecInfo->ScdId, RecInfo->ScdOqId);
        DmxHalClrScdCnt(RecInfo->ScdId);
        DmxHalEnableEsSCD(RecInfo->ScdId);
        DmxHalEnablePesSCD(RecInfo->ScdId);

        if (HI_UNF_DMX_REC_INDEX_TYPE_VIDEO == RecInfo->IndexType)
        {
            HI_INFO_DEMUX("VCodecType=%d\n", RecInfo->VCodecType);

            if (HI_UNF_VCODEC_TYPE_MPEG2 == RecInfo->VCodecType)
            {
                DmxHalSetScdFilter(0, 0x00);
                DmxHalChooseScdFilter(RecInfo->ScdId, 0);

                DmxHalSetScdFilter(1, 0xb3);
                DmxHalChooseScdFilter(RecInfo->ScdId, 1);
            }
            else if (HI_UNF_VCODEC_TYPE_AVS == RecInfo->VCodecType)
            {
                DmxHalSetScdFilter(1, 0xb3);
                DmxHalChooseScdFilter(RecInfo->ScdId, 1);

                DmxHalSetScdFilter(2, 0xb6);
                DmxHalChooseScdFilter(RecInfo->ScdId, 2);

                DmxHalSetScdRangeFilter(0, 0xb1, 0xb0);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 0);
            }
            else if (HI_UNF_VCODEC_TYPE_MPEG4 == RecInfo->VCodecType)
            {
                DmxHalSetScdFilter(3, 0xb0);
                DmxHalChooseScdFilter(RecInfo->ScdId, 3);

                DmxHalSetScdRangeFilter(1, 0xb6, 0xb5);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 1);

                DmxHalSetScdRangeFilter(2, 0x2f, 0x00);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 2);
            }
            else if (HI_UNF_VCODEC_TYPE_H264 == RecInfo->VCodecType)
            {
            #ifndef DMX_SCD_NEW_FLT_SUPPORT
                DmxHalSetScdFilter(4, 0x01);
                DmxHalChooseScdFilter(RecInfo->ScdId, 4);

                DmxHalSetScdFilter(5, 0x05);
                DmxHalChooseScdFilter(RecInfo->ScdId, 5);

                DmxHalSetScdRangeFilter(3, 0x08, 0x07);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 3);

                DmxHalSetScdFilter(6, 0x11);
                DmxHalChooseScdFilter(RecInfo->ScdId, 6);

                DmxHalSetScdFilter(7, 0x15);
                DmxHalChooseScdFilter(RecInfo->ScdId, 7);

                DmxHalSetScdRangeFilter(4, 0x18, 0x17);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 4);

                DmxHalSetScdFilter(8, 0x21);
                DmxHalChooseScdFilter(RecInfo->ScdId, 8);

                DmxHalSetScdFilter(9, 0x25);
                DmxHalChooseScdFilter(RecInfo->ScdId, 9);

                DmxHalSetScdRangeFilter(5, 0x28, 0x27);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 5);

                DmxHalSetScdRangeFilter(6, 0x78, 0x31);
                DmxHalChooseScdRangeFilter(RecInfo->ScdId, 6);
            #else
                DmxHalSetScdNewRangeFilter(0, 0xFF, 0x79, 0xFF, HI_TRUE);
                DmxHalSetScdNewRangeFilter(1, 0x01, 0x01, 0x1F, HI_FALSE);
                DmxHalSetScdNewRangeFilter(2, 0x05, 0x05, 0x1F, HI_FALSE);
                DmxHalSetScdNewRangeFilter(3, 0x08, 0x07, 0x1F, HI_FALSE);

                DmxHalChooseScdNewRangeFilter(RecInfo->ScdId, 0);
                DmxHalChooseScdNewRangeFilter(RecInfo->ScdId, 1);
                DmxHalChooseScdNewRangeFilter(RecInfo->ScdId, 2);
                DmxHalChooseScdNewRangeFilter(RecInfo->ScdId, 3);
            #endif
            }
        }

        DmxFqStart(RecInfo->ScdFqId);
        DmxOqStart(RecInfo->ScdOqId);
    }
#endif

    if (HI_UNF_DMX_REC_TYPE_ALL_PID == RecInfo->RecType)
    {
        DmxHalSetRecType(RecInfo->DmxId, DMX_REC_TYPE_ALL_TS);
        #ifdef DMX_REC_EXCLUDE_PID_SUPPORT
        DmxHalEnableAllRecExcludePid(RecInfo->DmxId);
        #endif
    }
    else
    {
        if (RecInfo->Descramed)
        {
            DmxHalSetRecType(RecInfo->DmxId, DMX_REC_TYPE_DESCRAM_TS);
        }
        else
        {
            DmxHalSetRecType(RecInfo->DmxId, DMX_REC_TYPE_SCRAM_TS);
        }
    }

    DmxHalSetRecTsCounter(RecInfo->DmxId, RecInfo->RecOqId);
    DmxHalSetRecTsCntReplace(RecInfo->DmxId);
    DmxHalSetSpsPauseType(RecInfo->DmxId, DMX_ENABLE);

    DmxHalSetTsRecBufId(RecInfo->DmxId, RecInfo->RecOqId);

    DmxFqStart(RecInfo->RecFqId);
    DmxOqStart(RecInfo->RecOqId);

    mutex_lock(&RecInfo->LockRec);
    RecInfo->FirstFrameMs   = 0;
    RecInfo->AddUpMs        = 0;
    RecInfo->LastScrClk     = 0;
    RecInfo->RecStatus      = DMX_REC_STATUS_START;

    RecInfo->PrevFrameEndAddr = 0;
    RecInfo->BlockStartAddr = FqInfo->u32BufPhyAddr;
    RecInfo->RemIdxCnt = 0;

    memset(&RecInfo->LastFrameInfo, 0, sizeof(HI_UNF_DMX_REC_INDEX_S));
    mutex_unlock(&RecInfo->LockRec);

    return HI_SUCCESS;
}

HI_S32 DMX_DRV_REC_StopRecChn(HI_U32 RecId)
{
    DMX_DEV_OSI_S  *DmxMgr      = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo     = &DmxMgr->DmxRecInfo[RecId];
    DMX_OQ_Info_S  *RecOqInfo   = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];
    Dmx_Rec_Data_Index_Helper *Entry, *Tmp;

    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_STOP == RecInfo->RecStatus)
    {
        return HI_SUCCESS;
    }

    mutex_lock(&RecInfo->LockRec);
    RecInfo->RecStatus = DMX_REC_STATUS_STOP;
    mutex_unlock(&RecInfo->LockRec);

    RecOqInfo->OqWakeUp = HI_TRUE;

    wake_up_interruptible(&RecOqInfo->OqWaitQueue);

#ifdef HI_DEMUX_SCD_SUPPORT
    if (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType)
    {
        DMX_OQ_Info_S  *ScdOqInfo = &DmxMgr->DmxOqInfo[RecInfo->ScdOqId];

        ScdOqInfo->OqWakeUp = HI_TRUE;

        wake_up_interruptible(&ScdOqInfo->OqWaitQueue);

        DmxFqStop(RecInfo->ScdFqId);
        DmxOqStop(RecInfo->ScdOqId);

        DmxHalDisableEsSCD(RecInfo->ScdId);
        DmxHalDisablePesSCD(RecInfo->ScdId);

        DmxHalScdFilterClear(RecInfo->ScdId);
        DmxHalScdRangeFilterClear(RecInfo->ScdId);
        DmxHalScdNewRangeFilterClear(RecInfo->ScdId);

        DmxHalSetSCDAttachChannel(RecInfo->ScdId, DMX_REC_SCD_INVALID_CHANNEL);
    }
#endif
    DmxFqStop(RecInfo->RecFqId);
    DmxOqStop(RecInfo->RecOqId);

    DmxRecFlush(RecId);
    DmxHalSetRecType(RecInfo->DmxId, DMX_REC_TYPE_NONE);
    #ifdef DMX_REC_EXCLUDE_PID_SUPPORT
    if (HI_UNF_DMX_REC_TYPE_ALL_PID == RecInfo->RecType)
    {
        DmxHalDisableAllRecExcludePid(RecInfo->DmxId);
    }
    #endif

    /* remove remain rec data&idx */
    RecInfo->RemIdxCnt = 0; 
    list_for_each_entry_safe(Entry, Tmp, &RecInfo->head, list)
    {
        list_del(&Entry->list);
        HI_KFREE(HI_ID_DEMUX, Entry);
    }

    return HI_SUCCESS;
}

HI_S32 DMX_DRV_REC_AcquireRecData(HI_U32 RecId, HI_U32 *PhyAddr, HI_U32 *KerAddr, HI_U32 *Len, HI_U32 Timeout)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = HI_NULL;
    DMX_OQ_Info_S  *OqInfo  = HI_NULL;
    HI_U32          Read;
    HI_U32          Write;

    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);

    if (Read == Write)
    {
        if (0 == Timeout)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_TRUE);

        OqInfo->OqWakeUp = HI_FALSE;

        wait_event_interruptible_timeout(OqInfo->OqWaitQueue, OqInfo->OqWakeUp, msecs_to_jiffies(Timeout));
        if (!OqInfo->OqWakeUp)
        {
            DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_FALSE);

            return HI_ERR_DMX_TIMEOUT;
        }

        OqInfo->OqWakeUp = HI_FALSE;

        DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);
    }

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }

    if (Read != Write)
    {
        HI_U32          CurrBlock   = OqInfo->u32OQVirAddr + Read* DMX_OQ_DESC_SIZE;
        OQ_DescInfo_S  *OqDesc      = (OQ_DescInfo_S*)CurrBlock;
        HI_U32          StartAddr   = OqDesc->start_addr;
        HI_U32          DataLen     = OqDesc->pvrctrl_datalen & 0xffff;

        OqInfo->u32ProcsBlk = Read + 1;
        if (OqInfo->u32ProcsBlk >= OqInfo->u32OQDepth)
        {
            OqInfo->u32ProcsBlk = 0;
        }

        *PhyAddr    = StartAddr;
        *KerAddr    = FqInfo->u32BufVirAddr + (StartAddr - FqInfo->u32BufPhyAddr);
        *Len        = DataLen;

        return HI_SUCCESS;
    }

    return HI_ERR_DMX_TIMEOUT;
}

HI_S32 DMX_DRV_REC_ReleaseRecData(HI_U32 RecId, HI_U32 PhyAddr, HI_U32 Len)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];

    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        HI_ERR_DEMUX("start rec channel first!");
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    return DmxOQRelease(RecInfo->RecFqId, RecInfo->RecOqId, PhyAddr, Len);
}

static HI_S32 DMX_GetRecDataIndex(HI_U32 RecId)
{
    HI_S32 ret = HI_FAILURE;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    HI_UNF_DMX_REC_INDEX_S *NewRecIndex = HI_NULL;
    HI_UNF_DMX_REC_DATA_S  *NewRecData = HI_NULL;
    HI_U32 NewFrameStartAddr, NewFrameEndAddr;  /* buffer inner addr */
    Dmx_Rec_Data_Index_Helper *helper;

    helper = HI_KMALLOC(HI_ID_DEMUX, sizeof(Dmx_Rec_Data_Index_Helper), GFP_KERNEL);
    if (helper)
    {
        memset(helper, 0, sizeof(Dmx_Rec_Data_Index_Helper));

        INIT_LIST_HEAD(&helper->list);
        NewRecData = &helper->data[0];
        NewRecIndex = &helper->index;
        
        ret = DMX_DRV_REC_AcquireRecIndex(RecId, NewRecIndex, 0);
        if (HI_SUCCESS == ret)
        {
            HI_U64 u64FrameGlobalOffset = NewRecIndex->u64GlobalOffset;

            NewFrameStartAddr = FqInfo->u32BufPhyAddr + do_div(u64FrameGlobalOffset, (HI_U64)FqInfo->u32BufSize);
            NewFrameEndAddr = NewFrameStartAddr + NewRecIndex->u32FrameSize; 
    			
            /* drop exception frame by simple rule. */
            if (unlikely(NewRecIndex->u32FrameSize >= FqInfo->u32BufSize || 0 == NewRecIndex->u32FrameSize))
            {
                HI_ERR_DEMUX("Drop exception frame(0x%x, 0x%x), fq(0x%x, 0x%x).\n", NewFrameStartAddr, NewRecIndex->u32FrameSize, 
                                            FqInfo->u32BufPhyAddr, FqInfo->u32BufSize);
                
                /* keep ret = success. */
                goto out1;
            }
    		
            /* skip verify first frame because it has no prev frame. */
            if (0 != RecInfo->PrevFrameEndAddr)
            {            
                /* in general, the phy address of adjacent frame is continuous, but this rule maybe broken by unstable stream(such as , weak signal). */
                if (unlikely(NewFrameStartAddr != RecInfo->PrevFrameEndAddr))
                {
                    HI_U32 Distance = NewFrameStartAddr > RecInfo->PrevFrameEndAddr ? 
                        NewFrameStartAddr - RecInfo->PrevFrameEndAddr : NewFrameStartAddr + FqInfo->u32BufSize - RecInfo->PrevFrameEndAddr;
                    
                    HI_WARN_DEMUX("Found discontinue(%d) frame data!\n", Distance);
                }
            }

            /* rewind frame */
            if ( NewFrameEndAddr > FqInfo->u32BufPhyAddr + FqInfo->u32BufSize)
            {
                HI_UNF_DMX_REC_DATA_S *NewRecData_2 = &helper->data[1];
                
                /* slice 1 */
                NewRecData->u32DataPhyAddr = NewFrameStartAddr;
                NewRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + NewRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr );
                NewRecData->u32Len = FqInfo->u32BufPhyAddr + FqInfo->u32BufSize - NewRecData->u32DataPhyAddr;

                /* slice 2 */
                NewRecData_2->u32DataPhyAddr = FqInfo->u32BufPhyAddr;
                NewRecData_2->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + NewRecData_2->u32DataPhyAddr - FqInfo->u32BufPhyAddr );
                NewRecData_2->u32Len = NewRecIndex->u32FrameSize - NewRecData->u32Len;

                helper->Rewind = HI_TRUE;
                helper->DataSel = 0;

                /* save and adjust frame end addr */
                RecInfo->PrevFrameEndAddr = NewRecData_2->u32DataPhyAddr + NewRecData_2->u32Len;
                RecInfo->PrevFrameEndAddr = (RecInfo->PrevFrameEndAddr == FqInfo->u32BufPhyAddr + FqInfo->u32BufSize) ?
                                                                    FqInfo->u32BufPhyAddr : RecInfo->PrevFrameEndAddr;
            }
            else
            {      
                NewRecData->u32DataPhyAddr = NewFrameStartAddr;
                NewRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + NewRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr );
                NewRecData->u32Len = NewRecIndex->u32FrameSize;

                helper->Rewind = HI_FALSE;
                helper->DataSel = 0;
                
                /* save and adjust frame end addr */
                RecInfo->PrevFrameEndAddr = NewFrameEndAddr;
                RecInfo->PrevFrameEndAddr = (RecInfo->PrevFrameEndAddr == FqInfo->u32BufPhyAddr + FqInfo->u32BufSize) ?
                                                                    FqInfo->u32BufPhyAddr : RecInfo->PrevFrameEndAddr;
            }

            list_add_tail(&helper->list, &RecInfo->head);
        }
        else
        {
            goto out1;
        }
    }
    else
    {
        HI_ERR_DEMUX("malloc rec data&idx helper failed.\n");
        ret = HI_ERR_DMX_ALLOC_MEM_FAILED;
        goto out0;
    }
    
    return HI_SUCCESS;
    
out1:
    HI_KFREE(HI_ID_DEMUX, helper);
out0:
    return ret;
}

static HI_VOID DMX_NewNoRewindFrame(DMX_RecInfo_S  *RecInfo, Dmx_Rec_Data_Index_Helper *Entry)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    HI_UNF_DMX_REC_DATA_S  *RemRecData = &RecInfo->RemRecData; 
    HI_UNF_DMX_REC_INDEX_S *RemRecIdx = HI_NULL; 

    /* data locate at before block */
    if (unlikely(RecInfo->BlockStartAddr >= Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len))
    {
        RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
        RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
        RemRecData->u32Len = FqInfo->u32BlockSize; 
    }
    else /* data locate at after block */
    {
        /* this frame bigger than block */
        if (Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr > FqInfo->u32BlockSize)
        {
            RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
            RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
            RemRecData->u32Len = FqInfo->u32BlockSize; 
        }
        else
        {
            RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
            RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
            RemRecData->u32Len = Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr; 

            BUG_ON(RemRecData->u32Len > FqInfo->u32BlockSize);

            RemRecIdx = &RecInfo->RemRecIdx[RecInfo->RemIdxCnt ++];
            memcpy(RemRecIdx, &Entry->index, sizeof(HI_UNF_DMX_REC_INDEX_S));

            list_del(&Entry->list);

            HI_KFREE(HI_ID_DEMUX, Entry);
        }
    }
}

static HI_VOID DMX_NewRewindFrame(DMX_RecInfo_S  *RecInfo, Dmx_Rec_Data_Index_Helper *Entry)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    HI_UNF_DMX_REC_DATA_S  *RemRecData = &RecInfo->RemRecData; 
    HI_UNF_DMX_REC_INDEX_S *RemRecIdx = HI_NULL; 

    if (0 == Entry->DataSel)  /* slice 1*/
    {
        BUG_ON(RecInfo->BlockStartAddr >= Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len);

        if (Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr >= FqInfo->u32BlockSize)
        {
            RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
            RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
            RemRecData->u32Len = FqInfo->u32BlockSize; 

            /* last block properly */
            if (RemRecData->u32DataPhyAddr + RemRecData->u32Len == FqInfo->u32BufPhyAddr + FqInfo->u32BufSize)
            {
                Entry->DataSel = 1;
            }
        }
        else
        {
            BUG();
        }
    }
    else /* slice 2*/
    {
        BUG_ON(RecInfo->BlockStartAddr >= Entry->data[1].u32DataPhyAddr + Entry->data[1].u32Len);

        /* this frame bigger than block */
        if (Entry->data[1].u32DataPhyAddr + Entry->data[1].u32Len - RecInfo->BlockStartAddr > FqInfo->u32BlockSize)
        {
            RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
            RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
            RemRecData->u32Len = FqInfo->u32BlockSize;
        }
        else
        {
            RemRecData->u32DataPhyAddr = RecInfo->BlockStartAddr;
            RemRecData->pDataAddr = (HI_U8  *)(FqInfo->u32BufVirAddr + RemRecData->u32DataPhyAddr - FqInfo->u32BufPhyAddr);
            RemRecData->u32Len = Entry->data[1].u32DataPhyAddr + Entry->data[1].u32Len - RecInfo->BlockStartAddr;

            BUG_ON(RemRecData->u32Len > FqInfo->u32BlockSize);

            RemRecIdx = &RecInfo->RemRecIdx[RecInfo->RemIdxCnt ++];
            memcpy(RemRecIdx, &Entry->index, sizeof(HI_UNF_DMX_REC_INDEX_S));

            list_del(&Entry->list);

            HI_KFREE(HI_ID_DEMUX, Entry);
        }
    }
}

static HI_VOID DMX_AppendNoRewindFrame(DMX_RecInfo_S  *RecInfo, Dmx_Rec_Data_Index_Helper *Entry)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    HI_UNF_DMX_REC_DATA_S  *RemRecData = &RecInfo->RemRecData; 
    HI_UNF_DMX_REC_INDEX_S *RemRecIdx = HI_NULL; 

    /* data locate at before block */
    if (unlikely(RecInfo->BlockStartAddr >= Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len))
    {
        RemRecData->u32Len = FqInfo->u32BlockSize; 
    }
    else /* data locate at after block */
    {
        /* this frame bigger than block */
        if (Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr > FqInfo->u32BlockSize)
        {
            RemRecData->u32Len = FqInfo->u32BlockSize; 
        }
        else
        {
            RemRecData->u32Len = Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr; 

            BUG_ON(RemRecData->u32Len > FqInfo->u32BlockSize);

            RemRecIdx = &RecInfo->RemRecIdx[RecInfo->RemIdxCnt ++];
            memcpy(RemRecIdx, &Entry->index, sizeof(HI_UNF_DMX_REC_INDEX_S));

            list_del(&Entry->list);

            HI_KFREE(HI_ID_DEMUX, Entry);
        }
    }
}

static HI_VOID DMX_AppendRewindFrame(DMX_RecInfo_S  *RecInfo, Dmx_Rec_Data_Index_Helper *Entry)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    HI_UNF_DMX_REC_DATA_S  *RemRecData = &RecInfo->RemRecData; 

    if (0 == Entry->DataSel)  /* slice 1*/
    {
        BUG_ON(RecInfo->BlockStartAddr >= Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len);

        if (Entry->data[0].u32DataPhyAddr + Entry->data[0].u32Len - RecInfo->BlockStartAddr >= FqInfo->u32BlockSize)
        {
            RemRecData->u32Len = FqInfo->u32BlockSize; 

            /* last block properly */
            if (RemRecData->u32DataPhyAddr + RemRecData->u32Len == FqInfo->u32BufPhyAddr + FqInfo->u32BufSize)
            {
                Entry->DataSel = 1;
            }
        }
        else
        {
            BUG();
        }
    }
    else /* slice 2 */
    {
        /* refer to DMX_NewRewindFrame slice 2 */
        BUG();     
    }
}

static HI_S32 DMX_AcquireRecDataIndex(HI_U32 RecId, HI_UNF_DMX_REC_DATA_INDEX_S *pstRecDataIdx)
{
    HI_S32 ret = HI_FAILURE;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    DMX_OQ_Info_S  *OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];
    HI_UNF_DMX_REC_DATA_S  *RemRecData = &RecInfo->RemRecData; 

    BUG_ON((RecInfo->BlockStartAddr - FqInfo->u32BufPhyAddr) % FqInfo->u32BlockSize);
 
    if (0 == RecInfo->RemIdxCnt)
    {
        memset(RemRecData, 0, sizeof(HI_UNF_DMX_REC_DATA_S));
    }

    while(RemRecData->u32Len < FqInfo->u32BlockSize)
    {        
        if (!list_empty(&RecInfo->head))
        {
            Dmx_Rec_Data_Index_Helper *Entry = list_first_entry(&RecInfo->head, Dmx_Rec_Data_Index_Helper, list);

            /* new */
            if (0 == RecInfo->RemIdxCnt)
            {
                if (HI_FALSE == Entry->Rewind)
                {
                    DMX_NewNoRewindFrame(RecInfo, Entry);
                }
                else /* rewind frame */
                {
                    DMX_NewRewindFrame(RecInfo, Entry);
                }
            }
            else if (RecInfo->RemIdxCnt < DMX_MAX_IDX_ACQUIRED_EACH_TIME) /* append */
            {
                if (HI_FALSE == Entry->Rewind)
                {
                    DMX_AppendNoRewindFrame(RecInfo, Entry);
                }
                else /* rewind frame */
                {
                    DMX_AppendRewindFrame(RecInfo, Entry);
                }
            }
            else
            {
                BUG();
            }
        }
        else /* there is no available index now in list */
        {
            /* almost overflow maybe clear and disabled in ISR, so we always enable it here. */
            DmxFqCheckOverflowInt(RecInfo->RecFqId); 
            if (unlikely(FqInfo->FqOverflowCount))
            {
                HI_ERR_DEMUX("Rec Buf(%d) Data overflowed.\n", RecInfo->RecFqId);   
                ret = HI_ERR_DMX_OVERFLOW_BUFFER;
                break;
            }
    
            ret = DMX_GetRecDataIndex(RecId);
            if (HI_SUCCESS != ret)
            {
                ret = HI_ERR_DMX_NOAVAILABLE_DATA;
                break;
            }
        }
    }  

    /* duplicate rec data and index  */
    if (RemRecData->u32Len == FqInfo->u32BlockSize)
    {
        pstRecDataIdx->u32RecDataCnt = 1;
        pstRecDataIdx->u32IdxNum = RecInfo->RemIdxCnt;
        
        memcpy(&pstRecDataIdx->stRecData[0], RemRecData, sizeof(HI_UNF_DMX_REC_DATA_S));

        if (sizeof(HI_UNF_DMX_REC_INDEX_S) * RecInfo->RemIdxCnt <= sizeof(RecInfo->RemRecIdx))
        {
            memcpy(&pstRecDataIdx->stIndex[0], &RecInfo->RemRecIdx[0], sizeof(HI_UNF_DMX_REC_INDEX_S) * RecInfo->RemIdxCnt);
        }
        else
        {
            BUG();
        }
        
        RecInfo->BlockStartAddr = RemRecData->u32DataPhyAddr + RemRecData->u32Len;

        OqInfo->u32ProcsBlk = (RecInfo->BlockStartAddr - FqInfo->u32BufPhyAddr)/FqInfo->u32BlockSize; 

        /* rewind buffer if possible */
        RecInfo->BlockStartAddr = (RecInfo->BlockStartAddr == FqInfo->u32BufPhyAddr + FqInfo->u32BufSize) ? 
                                                FqInfo->u32BufPhyAddr : RecInfo->BlockStartAddr ;
        RecInfo->RemIdxCnt = 0;

        ret = HI_SUCCESS;
    }
    
    return ret;
}

HI_S32 DMX_DRV_REC_AcquireRecDataIndex(HI_U32 RecId, HI_UNF_DMX_REC_DATA_INDEX_S *pstRecDataIdx)
{
    HI_S32 ret = HI_FAILURE;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];

    CHECKDMXID(RecInfo->DmxId);
    CHECKPOINTER(pstRecDataIdx);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        HI_ERR_DEMUX("start rec channel first!");
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    memset(pstRecDataIdx, 0x0, sizeof(HI_UNF_DMX_REC_DATA_INDEX_S));

    if (HI_UNF_DMX_REC_INDEX_TYPE_NONE == RecInfo->IndexType)
    {
        HI_U32 KerAddr = 0;
        HI_UNF_DMX_REC_DATA_S  *RecData  = &pstRecDataIdx->stRecData[0];       
        
        ret = DMX_DRV_REC_AcquireRecData(RecId, &RecData->u32DataPhyAddr, &KerAddr, &RecData->u32Len, 0);
        if ( HI_SUCCESS ==  ret)
        {
            pstRecDataIdx->u32IdxNum = 0;
            pstRecDataIdx->u32RecDataCnt = 1;
        }
    }
    else
    {
        ret = DMX_AcquireRecDataIndex(RecId, pstRecDataIdx);
    }

    return ret;  
}

HI_S32 DMX_DRV_REC_ReleaseRecDataIndex(HI_U32 RecId, HI_UNF_DMX_REC_DATA_INDEX_S *pstRecDataIdx)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = HI_NULL;
    DMX_OQ_Info_S  *OqInfo  = HI_NULL;
    OQ_DescInfo_S  *OqDesc;
    HI_U32 OqRead = 0;
    HI_U32 index;

    CHECKDMXID(RecInfo->DmxId);
    CHECKPOINTER(pstRecDataIdx);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        HI_ERR_DEMUX("start rec channel first!");
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];

    if (DMX_OQ_MODE_REC != OqInfo->enOQBufMode)
    {
        return HI_ERR_DMX_INVALID_PARA;
    }

    BUG_ON(pstRecDataIdx->u32RecDataCnt > 2);

    for (index = 0; index < pstRecDataIdx->u32RecDataCnt; index ++)
    {
        HI_UNF_DMX_REC_DATA_S  *RecData = &pstRecDataIdx->stRecData[index];
        HI_U32 RelBlkAddr = RecData->u32DataPhyAddr;

        while(RelBlkAddr < RecData->u32DataPhyAddr + RecData->u32Len)
        {
            DMXOsiOQGetReadWrite(OqInfo->u32OQId, HI_NULL, &OqRead);

            OqDesc = (OQ_DescInfo_S*)(OqInfo->u32OQVirAddr + OqRead * DMX_OQ_DESC_SIZE);

            if ((RelBlkAddr != OqDesc->start_addr) || ((OqDesc->pvrctrl_datalen & 0xffff) != FqInfo->u32BlockSize))
            {
                HI_ERR_DEMUX("Release block(0x%x) failed, current oq desc(0x%x, 0x%x)!\n", RelBlkAddr, OqDesc->start_addr, (OqDesc->pvrctrl_datalen & 0xffff));

                return HI_ERR_DMX_INVALID_PARA;
            }    

            /* only release one blk one time. */
            DmxOQReleaseByBlockCnt(RecInfo->RecFqId, OqInfo->u32OQId, 1);

            RelBlkAddr += FqInfo->u32BlockSize;
        }
    }
    
    return HI_SUCCESS;   
}

HI_S32 DMX_DRV_REC_AcquireRecIndex(HI_U32 RecId, HI_UNF_DMX_REC_INDEX_S *RecIndex, HI_U32 Timeout)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    HI_BOOL         bUseTimeStamp = HI_FALSE;
    DMX_FQ_Info_S  *FqInfo  = HI_NULL;
    DMX_OQ_Info_S  *OqInfo  = HI_NULL;
    HI_U32          Read;
    HI_U32          Write;
    HI_U32          jiffy   = msecs_to_jiffies(Timeout);
#ifndef HI_DEMUX_SCD_SUPPORT
    HI_UNF_DMX_REC_INDEX_S ThisFrame = {0};
    HI_U32 ScrClk;
#else
    HI_S32          ret     = HI_FAILURE;
    HI_U32          KerAddr;
#endif
    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        HI_ERR_DEMUX("start rec channel first!");
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    if (HI_UNF_DMX_REC_INDEX_TYPE_NONE == RecInfo->IndexType)
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }
	
	if ( RecInfo->enRecTimeStamp >= DMX_REC_TIMESTAMP_ZERO)
	{
	    bUseTimeStamp = HI_TRUE;
	}

#ifndef HI_DEMUX_SCD_SUPPORT
    FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];
    OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->RecOqId];

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);
    
    if (Read == Write)
    {
        if (0 == jiffy)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_TRUE);

        OqInfo->OqWakeUp = HI_FALSE;

        wait_event_interruptible_timeout(OqInfo->OqWaitQueue, OqInfo->OqWakeUp, jiffy);
        if (!OqInfo->OqWakeUp)
        {
            DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_FALSE);

            return HI_ERR_DMX_TIMEOUT;
        }

        OqInfo->OqWakeUp = HI_FALSE;

        DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);
    }

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }
    /* calculate the tsoffset by the read write point of oq */
    if (Read != Write)
    {
        /* only write pointer changed means new data comming */
        if (RecInfo->u32LastWrite != Write)
        {
            /* write changed from 0 to 95,so normally the fq queue length is 96, 
        	    but the FqInfo->u32FQDepth is 97 for overflow,we need subtract 1 for the queue length 96*/
            /* write pointer changed, we need to use the subtract between last write and current write to get offset  */
            RecInfo->u64TsOffset += 
            ((Write-RecInfo->u32LastWrite+FqInfo->u32FQDepth-1)%(FqInfo->u32FQDepth-1))*FqInfo->u32BlockSize;
        }
        else
        {
            HI_WARN_DEMUX("no rec data enter!\n");
            //HI_ERR_DEMUX("Read:%d Write:%d LastRead:%d LastWrite:%d Offset:%llx FqDepth %d!\n", Read, 
            // Write, RecInfo->u32LastRead, RecInfo->u32LastWrite,  RecInfo->u64TsOffset, FqInfo->u32FQDepth);
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }
        
        RecInfo->u32LastWrite = Write;

        OqInfo->u32ProcsBlk = Read + 1;
        if (OqInfo->u32ProcsBlk >= OqInfo->u32OQDepth)
        {
            OqInfo->u32ProcsBlk = 0;
        }
    }

    ThisFrame.u64GlobalOffset = RecInfo->u64TsOffset;
    
    DmxHalGetCurrentSCR(&ScrClk);
    
	 /*time counter overflow*/
    if (ScrClk < RecInfo->LastScrClk)
    {
        RecInfo->AddUpMs += DMX_MAX_SCR_MS;
    }
    RecInfo->LastScrClk = ScrClk;
    /*adjust every index's u32DataTimeMs, make it begin from a value near  0*/
    ThisFrame.u32DataTimeMs = ScrClk / 90 + RecInfo->AddUpMs - RecInfo->FirstFrameMs;
    ThisFrame.u32FrameSize = 0; /*this frame size can  be calc only the next frame come*/
    ThisFrame.u32PtsMs     = RecInfo->LastFrameInfo.u32PtsMs; /*if there is no last frame?*/
    ThisFrame.enFrameType  = HI_UNF_FRAME_TYPE_UNKNOWN; /*we set HI_UNF_FRAME_TYPE_UNKNOWN as scrambled stream's index*/ 

    /*then we start generate index by software ,otherwise ,
    inorder to avoid some duration of stream loss (can not be read/playack)*/

    /*every DMX_REC_SOFT_INDEX_TIME_GAP ,we generate a soft index ,except the first time clear*/
    if ( (ThisFrame.u32DataTimeMs - RecInfo->LastFrameInfo.u32DataTimeMs >= DMX_REC_SOFT_INDEX_TIME_GAP) 
          || (RecInfo->LastFrameInfo.enFrameType != HI_UNF_FRAME_TYPE_UNKNOWN))
    {                       
        /*if Record just starting,we just copy this frame into lastframe ,but not output it ,
        as we can not get ThisFrame.u32FrameSize until the next index generated. */            
        if ( 0 == RecInfo->FirstFrameMs)
        {
            RecInfo->FirstFrameMs = ThisFrame.u32DataTimeMs; 
            RecInfo->PrevFrameMs = ThisFrame.u32DataTimeMs; 
            
            memcpy(&RecInfo->LastFrameInfo, &ThisFrame, sizeof(HI_UNF_DMX_REC_INDEX_S));
            RecInfo->LastFrameInfo.u32DataTimeMs = 0;/*set the first index as time 0*/
            RecInfo->LastFrameInfo.u64GlobalOffset = 0;/*set the first index's offset as 0, for align 188 */
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }
        
        RecInfo->LastFrameInfo.u32FrameSize = ThisFrame.u64GlobalOffset - RecInfo->LastFrameInfo.u64GlobalOffset;

        memcpy(RecIndex, &RecInfo->LastFrameInfo, sizeof(HI_UNF_DMX_REC_INDEX_S));

        RecInfo->PrevFrameMs = RecIndex->u32DataTimeMs;
        
        memcpy(&RecInfo->LastFrameInfo, &ThisFrame, sizeof(HI_UNF_DMX_REC_INDEX_S));
        
        return  HI_SUCCESS;
    }
    else 
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }
#else

    FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->ScdFqId];
    OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->ScdOqId];

    do
    {
        DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);

        if (Read == Write)
        {
            if (0 == jiffy)
            {
                return HI_ERR_DMX_NOAVAILABLE_DATA;
            }

            DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_TRUE);

            OqInfo->OqWakeUp = HI_FALSE;

            jiffy = wait_event_interruptible_timeout(OqInfo->OqWaitQueue, OqInfo->OqWakeUp, jiffy);
            if (!OqInfo->OqWakeUp)
            {
                DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_FALSE);

                return HI_ERR_DMX_TIMEOUT;
            }

            OqInfo->OqWakeUp = HI_FALSE;

            DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);
        }

        if (DMX_REC_STATUS_START != RecInfo->RecStatus)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        if (Read != Write)
        {
            HI_UNF_DMX_REC_INDEX_S *CurrFrame   = &RecInfo->LastFrameInfo;
            HI_UNF_DMX_REC_INDEX_S  ThisFrame = {0};
            HI_U32                  CurrBlock   = OqInfo->u32OQVirAddr + Read * DMX_OQ_DESC_SIZE;
            OQ_DescInfo_S          *OqDesc      = (OQ_DescInfo_S*)CurrBlock;
            HI_U32                  PhyAddr     = OqDesc->start_addr;
            HI_U32                  DataLen     = OqDesc->pvrctrl_datalen & 0xffff;
            FINDEX_SCD_S            EsScd;
            DMX_IDX_DATA_S         *ScData;
            HI_U32                  ScCount;
            HI_U32                  i;

            KerAddr = FqInfo->u32BufVirAddr + (PhyAddr - FqInfo->u32BufPhyAddr) + OqInfo->u32ProcsOffset;

            ScData  = (DMX_IDX_DATA_S*)KerAddr;
            ScCount = (DataLen - OqInfo->u32ProcsOffset)/ sizeof(DMX_IDX_DATA_S);

            if (HI_UNF_DMX_REC_INDEX_TYPE_VIDEO == RecInfo->IndexType)
            {
                for (i = 0; i < ScCount; i++)
                {
                    HI_U32 SrcClk = ScData->u32SrcClk;

                    if (HI_SUCCESS == DmxScdToVideoIndex(bUseTimeStamp,ScData++, &EsScd))
                    {
                        HI_U64 GlobalOffset = CurrFrame->u64GlobalOffset;

                        FIDX_FeedStartCode(RecInfo->PicParser, &EsScd);

                        if (GlobalOffset != CurrFrame->u64GlobalOffset)
                        {
                            CurrFrame->u32DataTimeMs = SrcClk / 90;

                            memcpy(RecIndex, CurrFrame, sizeof(HI_UNF_DMX_REC_INDEX_S));

                            ret = HI_SUCCESS;

                            break;
                        }
                    }
                }
            }
            else
            {
                for (i = 0; i < ScCount; i++)
                {
                    if (HI_SUCCESS == DmxScdToAudioIndex(&ThisFrame, ScData++))
                    {
                        /*only for the first scd (index) will enter this condition */
                        if ( RecInfo->LastFrameInfo.u32DataTimeMs == 0 )
                        {
                            memcpy(&RecInfo->LastFrameInfo, &ThisFrame, sizeof(HI_UNF_DMX_REC_INDEX_S));
                            continue;
                        }
                        
                        RecInfo->LastFrameInfo.u32FrameSize = ThisFrame.u64GlobalOffset - RecInfo->LastFrameInfo.u64GlobalOffset;                  
                        memcpy(RecIndex, &RecInfo->LastFrameInfo, sizeof(HI_UNF_DMX_REC_INDEX_S));
                        memcpy(&RecInfo->LastFrameInfo, &ThisFrame, sizeof(HI_UNF_DMX_REC_INDEX_S));

                        ret = HI_SUCCESS;

                        break;
                    }
                }
            }

            OqInfo->u32ProcsOffset += (HI_U32)ScData - KerAddr;
            if (OqInfo->u32ProcsOffset == DataLen)
            {
                DmxOQRelease(OqInfo->u32FQId, OqInfo->u32OQId, PhyAddr, DataLen);
            }

            if (HI_SUCCESS == ret)
            {
                BUG_ON(RecIndex->u32DataTimeMs > DMX_MAX_SCR_MS);
                 
                if (0 == RecInfo->FirstFrameMs)
                {
                    RecInfo->FirstFrameMs = RecIndex->u32DataTimeMs;
                    RecInfo->PrevFrameMs = RecIndex->u32DataTimeMs;
                }     

                if (RecIndex->u32DataTimeMs < RecInfo->PrevFrameMs)
                {
                    RecInfo->AddUpMs += DMX_MAX_SCR_MS;
                }
           
                RecInfo->PrevFrameMs = RecIndex->u32DataTimeMs;
                
                /* adjust total data time ms according to overflow numbers */  
                RecIndex->u32DataTimeMs += (RecInfo->AddUpMs - RecInfo->FirstFrameMs);
                
                return HI_SUCCESS;
            }
            else
            {
                return HI_ERR_DMX_NOAVAILABLE_DATA;
            }
        }
    } while (jiffy);

    return HI_ERR_DMX_TIMEOUT;
#endif
}

HI_S32 DMX_DRV_REC_GetRecBufferStatus(HI_U32 RecId, HI_UNF_DMX_RECBUF_STATUS_S *BufStatus)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->RecFqId];

    CHECKDMXID(RecInfo->DmxId);

    BufStatus->u32BufSize   = FqInfo->u32BufSize;
    BufStatus->u32UsedSize  = 0;

    if (DMX_REC_STATUS_START == RecInfo->RecStatus)
    {
        HI_U32  Read    = DmxHalGetFQReadPtr(RecInfo->RecFqId);
        HI_U32  Write   = DmxHalGetFQWritePtr(RecInfo->RecFqId);

        if (Read < Write)
        {
            BufStatus->u32UsedSize = (FqInfo->u32FQDepth - 1 - Write + Read) * FqInfo->u32BlockSize;
        }
        else if (Read > Write)
        {
            BufStatus->u32UsedSize = (Read - Write) * FqInfo->u32BlockSize;
        }
    }

    return HI_SUCCESS;
}

#ifdef HI_DEMUX_SCD_SUPPORT
HI_S32 DMX_DRV_REC_AcquireScdData(HI_U32 RecId, HI_U32 *PhyAddr, HI_U32 *KerAddr, HI_U32 *Len, HI_U32 Timeout)
{
    HI_S32          ret;
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];
    DMX_FQ_Info_S  *FqInfo  = HI_NULL;
    DMX_OQ_Info_S  *OqInfo  = HI_NULL;
    HI_U32          Read;
    HI_U32          Write;

    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    FqInfo  = &DmxMgr->DmxFqInfo[RecInfo->ScdFqId];
    OqInfo  = &DmxMgr->DmxOqInfo[RecInfo->ScdOqId];

    DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);

    if (Read == Write)
    {
        if (0 == Timeout)
        {
            return HI_ERR_DMX_NOAVAILABLE_DATA;
        }

        DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_TRUE);

        OqInfo->OqWakeUp = HI_FALSE;

        ret = wait_event_interruptible_timeout(OqInfo->OqWaitQueue, OqInfo->OqWakeUp, msecs_to_jiffies(Timeout));
        if (!OqInfo->OqWakeUp)
        {
            DmxHalOQEnableOutputInt(OqInfo->u32OQId, HI_FALSE);

            return HI_ERR_DMX_TIMEOUT;
        }

        OqInfo->OqWakeUp = HI_FALSE;

        DMXOsiOQGetReadWrite(OqInfo->u32OQId, &Write, &Read);
    }

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOAVAILABLE_DATA;
    }

    if (Read != Write)
    {
        HI_U32          CurrBlock   = OqInfo->u32OQVirAddr + Read * DMX_OQ_DESC_SIZE;
        OQ_DescInfo_S  *OqDesc      = (OQ_DescInfo_S*)CurrBlock;
        HI_U32          StartAddr   = OqDesc->start_addr;
        HI_U32          DataLen     = OqDesc->pvrctrl_datalen & 0xffff;

        OqInfo->u32ProcsBlk = Read + 1;
        if (OqInfo->u32ProcsBlk >= OqInfo->u32OQDepth)
        {
            OqInfo->u32ProcsBlk = 0;
        }

        *PhyAddr    = StartAddr;
        *KerAddr    = FqInfo->u32BufVirAddr + (StartAddr - FqInfo->u32BufPhyAddr);
        *Len        = DataLen;

        return HI_SUCCESS;
    }

    return HI_ERR_DMX_TIMEOUT;
}

HI_S32 DMX_DRV_REC_ReleaseScdData(HI_U32 RecId, HI_U32 PhyAddr, HI_U32 Len)
{
    DMX_DEV_OSI_S  *DmxMgr  = g_pDmxDevOsi;
    DMX_RecInfo_S  *RecInfo = &DmxMgr->DmxRecInfo[RecId];

    CHECKDMXID(RecInfo->DmxId);

    if (DMX_REC_STATUS_START != RecInfo->RecStatus)
    {
        return HI_ERR_DMX_NOT_START_REC_CHAN;
    }

    return DmxOQRelease(RecInfo->ScdFqId, RecInfo->ScdOqId, PhyAddr, Len);
}
#endif
#endif

HI_S32 DMX_OsiDeviceInit(HI_U32 PoolBufSize, HI_U32 BlockSize)
{
    HI_S32 ret;

    ret = DMX_OsiInit(PoolBufSize, BlockSize);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    if (0 != osal_request_irq(DMX_INT_NUM, (irq_handler_t) hi_dmx_irq, IRQF_SHARED, "dmx", (HI_VOID*)g_pDmxDevOsi))
    {
        DMX_OsiDeInit();
        HI_FATAL_DEMUX("osal_request_irq error\n");
        return HI_FAILURE;
    }

    DmxHalEnableAllPVRInt();

#ifdef HI_DEMUX_REC_SUPPORT
    FIDX_Init(DmxRecUpdateFrameInfo);
#endif

    return HI_SUCCESS;
}

HI_VOID DMX_OsiDeviceDeInit(HI_VOID)
{
    DmxHalDisableAllPVRInt();
    osal_free_irq(DMX_INT_NUM, "dmx", (HI_VOID*)g_pDmxDevOsi);
    DMX_OsiDeInit();
}

/*****************************************************************************
 Prototype    : DMX_OsiSuspend
 Description  : Demux
 Input        : None
 Output       : None
 Return Value : None
*****************************************************************************/
HI_S32 DMX_OsiSuspend(PM_BASEDEV_S *himd, pm_message_t state)
{
    DMX_DEV_OSI_S  *DmxDevOsi   = g_pDmxDevOsi;
    HI_U32          i;

    del_timer_sync(&CheckChnTimeoutTimer);

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &DmxDevOsi->DmxChanInfo[i];

        if (ChanInfo->DmxId < DMX_CNT)
        {
            if (HI_UNF_DMX_CHAN_CLOSE != ChanInfo->ChanStatus)
            {
                HI_UNF_DMX_CHAN_STATUS_E ChanStatus = ChanInfo->ChanStatus;

                DMX_OsiCloseChannel(i);

                ChanInfo->ChanStatus = ChanStatus;
            }
        }
    }

    HI_PRINT("DEMUX suspend OK\n");

    return HI_SUCCESS;
}

/*****************************************************************************
 Prototype    : DMX_OsiResume
 Description  : Demux
 Input        : None
 Output       : None
 Return Value : None
 Others       :
*****************************************************************************/
HI_S32 DMX_OsiResume(PM_BASEDEV_S *himd)
{
    DMX_DEV_OSI_S          *DmxDevOsi   = g_pDmxDevOsi;
    HI_UNF_DMX_PORT_ATTR_S  PortAttr;
    HI_U32                  i;
    HI_U32                  j;
	HI_S32					ret;

    if (HI_SUCCESS != DmxReset())
    {
        return HI_FAILURE;
    }

#ifdef DMX_DESCRAMBLER_SUPPORT

	if ( g_pDscFunc != NULL )
	{
#ifdef CHIP_TYPE_hi3716m
	    if (Hi3716MV300Flag)
	    {
	        g_pDscFunc->DescInitHardFlag();
	    }
#else
	    #ifdef DMX_DESCRAMBLER_VERSION_1
	    g_pDscFunc->DescInitHardFlag();
	    #endif
#endif
	}

#endif

    mutex_init(&DmxDevOsi->lock_Channel);
    mutex_init(&DmxDevOsi->lock_AVChan);
    mutex_init(&DmxDevOsi->lock_Filter);
    mutex_init(&DmxDevOsi->lock_Key);
    spin_lock_init(&DmxDevOsi->splock_OqBuf);

    DmxFqStart(DMX_FQ_COMMOM);

    for (i = 0; i < DMX_TUNERPORT_CNT; i++)
    {
        DMX_TunerPort_Info_S *PortInfo = &DmxDevOsi->TunerPortInfo[i];

        PortAttr.enPortMod              = HI_UNF_DMX_PORT_MODE_EXTERNAL;
        PortAttr.enPortType             = PortInfo->PortType;
        PortAttr.u32SyncLostTh          = PortInfo->SyncLostTh;
        PortAttr.u32SyncLockTh          = PortInfo->SyncLockTh;
        PortAttr.u32TunerInClk          = PortInfo->TunerInClk;
        PortAttr.u32SerialBitSelector   = PortInfo->BitSelector;
        PortAttr.u32TunerErrMod         = 0;
        PortAttr.u32UserDefLen1         = 0;
        PortAttr.u32UserDefLen2         = 0;

        DMX_OsiTunerPortSetAttr(i, &PortAttr);

        DmxHalDvbPortSetTsCountCtrl(i, TS_COUNT_CRTL_START);
        DmxHalDvbPortSetErrTsCountCtrl(i, TS_COUNT_CRTL_START);
    }

    for (i = 0; i < DMX_RAMPORT_CNT; i++)
    {
        DMX_RamPort_Info_S *PortInfo = &DmxDevOsi->RamPortInfo[i];

        init_waitqueue_head(&PortInfo->WaitQueue);

        PortAttr.enPortMod              = HI_UNF_DMX_PORT_MODE_RAM;
        PortAttr.enPortType             = PortInfo->PortType;
        PortAttr.u32SyncLostTh          = PortInfo->SyncLostTh;
        PortAttr.u32SyncLockTh          = PortInfo->SyncLockTh;
        PortAttr.u32TunerInClk          = 0;
        PortAttr.u32SerialBitSelector   = 0;
        PortAttr.u32TunerErrMod         = 0;
        PortAttr.u32UserDefLen1         = PortInfo->MinLen;
        PortAttr.u32UserDefLen2         = PortInfo->MaxLen;

        DMX_OsiRamPortSetAttr(i, &PortAttr);

        DmxHalIPPortEnableInt(i);
        DmxHalIPPortSetIntCnt(i, DMX_DEFAULT_INT_CNT);

        DmxHalIPPortSetTsCountCtrl(i, HI_TRUE);

        if (PortInfo->PhyAddr)
        {
            PortInfo->DescCurrAddr  = (HI_U32*)PortInfo->DescKerAddr;
            PortInfo->DescWrite     = 0;
            PortInfo->Read          = 0;
            PortInfo->Write         = 0;
            PortInfo->ReqAddr       = 0;
            PortInfo->ReqLen        = 0;
            PortInfo->WaitLen       = 0;
            PortInfo->WakeUp        = HI_FALSE;

            spin_lock_init(&PortInfo->LockRamPort);
            
            PortInfo->GetCount      = 0;
            PortInfo->GetValidCount = 0;
            PortInfo->PutCount      = 0;

            TsBufferConfig(i, HI_TRUE, PortInfo->DescPhyAddr, PortInfo->DescDepth);
        }
    }

    for (i = 0; i < DMX_CNT; i++)
    {
        DMX_Sub_DevInfo_S  *DmxInfo = &DmxDevOsi->SubDevInfo[i];
        DMX_RecInfo_S      *RecInfo = &DmxDevOsi->DmxRecInfo[i];

        DMX_OsiAttachPort(i, DmxInfo->PortMode, DmxInfo->PortId);

        DmxHalSetSpsRefRecCh(i, 0xff);

        if (DMX_REC_STATUS_START != RecInfo->RecStatus)
        {
            continue;
        }

        RecInfo->RecStatus = DMX_REC_STATUS_STOP;

        DMX_DRV_REC_StartRecChn(i);
    }

#ifdef DMX_DESCRAMBLER_SUPPORT
	if ( g_pDscFunc != NULL )
	{
		g_pDscFunc->DmxDescramblerResume();
	}
#endif

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &DmxDevOsi->DmxChanInfo[i];

        if (ChanInfo->DmxId < DMX_CNT)
        {
            DmxSetChannel(i);

        #ifdef DMX_DESCRAMBLER_SUPPORT
            if (ChanInfo->KeyId < DMX_KEY_CNT)
            {
                HI_U32 KeyId = ChanInfo->KeyId;

                ChanInfo->KeyId = DMX_INVALID_KEY_ID;
				if ( g_pDscFunc != NULL )
				{
				    g_pDscFunc->DMX_OsiDescramblerAttach(KeyId, ChanInfo->ChanId);
				}
				else
				{
				    HI_ERR_DEMUX("error:g_pDscFunc have not beed registered\n");
				}

                
            }
        #endif

            if (HI_UNF_DMX_CHAN_CLOSE != ChanInfo->ChanStatus)
            {
                ChanInfo->ChanStatus = HI_UNF_DMX_CHAN_CLOSE;

                ret = DMX_OsiOpenChannel(i);
				if (ret != HI_SUCCESS)
				{
					return ret;
            	}
            }
        }
        else
        {
            DmxHalSetChannelPlayDmxid(i, DMX_INVALID_DEMUX_ID);
            DmxHalSetChannelRecDmxid(i, DMX_INVALID_DEMUX_ID);
            DmxHalSetChannelPid(i, DMX_INVALID_PID);
        }
    }

    for (i = 0; i < DMX_FILTER_CNT; i++)
    {
        DMX_FilterInfo_S *FilterInfo = &g_pDmxDevOsi->DmxFilterInfo[i];

        if (DMX_INVALID_FILTER_ID != FilterInfo->FilterId)
        {
            HI_UNF_DMX_FILTER_ATTR_S FilterAttr;

            FilterAttr.u32FilterDepth = FilterInfo->Depth;

            for (j = 0; j < FilterInfo->Depth; j++)
            {
                FilterAttr.au8Match[j]  = FilterInfo->Match[j];
                FilterAttr.au8Mask[j]   = FilterInfo->Mask[j];
                FilterAttr.au8Negate[j] = FilterInfo->Negate[j];
            }

            DMX_OsiSetFilterAttr(i, &FilterAttr);

            if (DMX_INVALID_CHAN_ID != FilterInfo->ChanId)
            {
                HI_U32 ChanId = FilterInfo->ChanId;

                FilterInfo->ChanId = DMX_INVALID_CHAN_ID;

                DMX_OsiAttachFilter(i, ChanId);
            }
        }
    }

    for (i = 0; i < DMX_PCR_CHANNEL_CNT; i++)
    {
        DMX_PCR_Info_S *PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[i];

        if (PcrInfo->DmxId < DMX_CNT)
        {
            DMX_OsiPcrChannelSetPid(i, PcrInfo->PcrPid);
        }
    }

    DmxHalEnableAllDavInt();
    DmxHalEnableAllChEopInt();
    DmxHalEnableAllChEnqueInt();
    DmxHalFQEnableAllOverflowInt();

    DmxHalSetFlushMaxWaitTime(0x2400);
    DmxHalSetDataFakeMod(DMX_ENABLE);
    init_waitqueue_head(&DmxDevOsi->DmxWaitQueue);

    DmxHalEnableAllPVRInt();

    init_waitqueue_head(&AllChnWaitQueue);

    mod_timer(&CheckChnTimeoutTimer, jiffies + msecs_to_jiffies(DMX_CHECKCHN_TIMEOUT));

    HI_PRINT("DEMUX resume OK\n");

    return HI_SUCCESS;
}

#ifdef HI_DEMUX_PROC_SUPPORT
HI_S32 DMX_OsiRamPortGetBufInfo(HI_U32 PortId, DMX_Proc_RamPort_BufInfo_S *BufInfo)
{
    HI_S32              ret         = HI_FAILURE;
    DMX_RamPort_Info_S *PortInfo    = &g_pDmxDevOsi->RamPortInfo[PortId];

    if (PortInfo->PhyAddr)
    {
        BufInfo->PhyAddr        = PortInfo->PhyAddr;
        BufInfo->BufSize        = PortInfo->BufSize;
        BufInfo->Read           = PortInfo->Read;
        BufInfo->Write          = PortInfo->Write;
        BufInfo->UsedSize       = GetQueueLenth(BufInfo->Read, BufInfo->Write, BufInfo->BufSize);
        BufInfo->GetCount       = PortInfo->GetCount;
        BufInfo->GetValidCount  = PortInfo->GetValidCount;
        BufInfo->PutCount       = PortInfo->PutCount;

        ret = HI_SUCCESS;
    }

    return ret;
}

HI_S32 DMX_OsiGetDmxRecProc(HI_U32 RecId, DMX_Proc_Rec_BufInfo_S *BufInfo)
{
    HI_S32          ret     = HI_FAILURE;
    DMX_RecInfo_S  *RecInfo = &g_pDmxDevOsi->DmxRecInfo[RecId];

    if (RecInfo->DmxId < DMX_CNT)
    {
        switch (RecInfo->RecType)
        {
            case HI_UNF_DMX_REC_TYPE_SELECT_PID :
            case HI_UNF_DMX_REC_TYPE_ALL_PID :
            {
                DMX_FQ_Info_S *FqInfo = &g_pDmxDevOsi->DmxFqInfo[RecInfo->RecFqId];

                BufInfo->RecType    = RecInfo->RecType;
                BufInfo->Descramed  = RecInfo->Descramed;
                BufInfo->BlockCnt   = FqInfo->u32FQDepth - 1;
                BufInfo->BlockSize  = FqInfo->u32BlockSize;
                BufInfo->RecStatus  = (DMX_REC_STATUS_START == RecInfo->RecStatus) ? 1 : 0;
                BufInfo->Overflow   = FqInfo->FqOverflowCount;

                DMXOsiOQGetReadWrite(RecInfo->RecOqId, &BufInfo->BufWrite, &BufInfo->BufRead);

                ret = HI_SUCCESS;

                break;
            }

            default :
                ret = HI_FAILURE;
        }
    }

    return ret;
}

HI_S32 DMX_OsiGetDmxRecScdProc(HI_U32 RecId, DMX_Proc_RecScd_BufInfo_S *BufInfo)
{
    HI_S32          ret     = HI_FAILURE;
    DMX_RecInfo_S  *RecInfo = &g_pDmxDevOsi->DmxRecInfo[RecId];

    if (RecInfo->DmxId < DMX_CNT)
    {
        memset(BufInfo, 0, sizeof(DMX_Proc_RecScd_BufInfo_S));

        switch (RecInfo->RecType)
        {
            case HI_UNF_DMX_REC_TYPE_SELECT_PID :
            {
                BufInfo->IndexType  = RecInfo->IndexType;
                BufInfo->IndexPid   = DMX_INVALID_PID;

                if (HI_UNF_DMX_REC_INDEX_TYPE_NONE != RecInfo->IndexType)
                {
                    DMX_FQ_Info_S *FqInfo = &g_pDmxDevOsi->DmxFqInfo[RecInfo->ScdFqId];

                    BufInfo->IndexPid   = RecInfo->IndexPid;
                    BufInfo->BlockCnt   = FqInfo->u32FQDepth - 1;
                    BufInfo->BlockSize  = FqInfo->u32BlockSize;
                    BufInfo->Overflow   = FqInfo->FqOverflowCount;

                    DMXOsiOQGetReadWrite(RecInfo->ScdOqId, &BufInfo->BufWrite, &BufInfo->BufRead);
                }

                ret = HI_SUCCESS;

                break;
            }

            case HI_UNF_DMX_REC_TYPE_ALL_PID :
            {
                BufInfo->IndexType  = HI_UNF_DMX_REC_INDEX_TYPE_NONE;
                BufInfo->IndexPid   = DMX_INVALID_PID;

                ret = HI_SUCCESS;

                break;
            }

            default :
                ret = HI_FAILURE;
        }
    }

    return ret;
}

DMX_ChanInfo_S* DMX_OsiGetChannelProc(HI_U32 ChanId)
{
    DMX_ChanInfo_S *ChanInfo;

    ChanInfo = &g_pDmxDevOsi->DmxChanInfo[ChanId];
    if (ChanInfo->DmxId >= DMX_CNT)
    {
        ChanInfo = HI_NULL;
    }

    return ChanInfo;
}

HI_S32 DMX_OsiGetChanBufProc(HI_U32 ChanId, DMX_Proc_ChanBuf_S *BufInfo)
{
    DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[ChanId];

    if (ChanInfo->DmxId >= DMX_CNT)
    {
        return HI_FAILURE;
    }

    memset(BufInfo, 0, sizeof(DMX_Proc_ChanBuf_S));

    if (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY == (HI_UNF_DMX_CHAN_OUTPUT_MODE_PLAY & ChanInfo->ChanOutMode))
    {
        DMX_OQ_Info_S  *OqInfo  = &g_pDmxDevOsi->DmxOqInfo[ChanInfo->ChanOqId];
        DMX_FQ_Info_S  *FqInfo  = &g_pDmxDevOsi->DmxFqInfo[OqInfo->u32FQId];

        DMXOsiOQGetReadWrite(ChanInfo->ChanOqId, &BufInfo->DescWrite, &BufInfo->DescRead);

        BufInfo->DescDepth  = OqInfo->u32OQDepth;
        BufInfo->BlockSize  = FqInfo->u32BlockSize;
        BufInfo->Overflow   = FqInfo->FqOverflowCount;
    }

    return HI_SUCCESS;
}

DMX_FilterInfo_S* DMX_OsiGetFilterProc(HI_U32 FilterId)
{
    DMX_FilterInfo_S *FilterInfo;

    FilterInfo = &g_pDmxDevOsi->DmxFilterInfo[FilterId];
    if (DMX_INVALID_FILTER_ID == FilterInfo->FilterId)
    {
        FilterInfo = HI_NULL;
    }

    return FilterInfo;
}

DMX_PCR_Info_S* DMX_OsiGetPcrChannelProc(const HI_U32 PcrId)
{
    DMX_PCR_Info_S *PcrInfo;

    PcrInfo = &g_pDmxDevOsi->DmxPcrInfo[PcrId];
    if (PcrInfo->DmxId >= DMX_CNT)
    {
        PcrInfo = HI_NULL;
    }

    return PcrInfo;
}

static HI_S32 DMX_OsiAddRecPid(HI_U32 DmxId,HI_U32 Pid, HI_U32 *ChanId)
{
    HI_S32                  ret;
    HI_UNF_DMX_CHAN_ATTR_S  ChanAttr;
    DMX_MMZ_BUF_S           ChanBuf;

    ChanAttr.u32BufSize     = 0x4000;
    ChanAttr.enCRCMode      = HI_UNF_DMX_CHAN_CRC_MODE_FORBID;
    ChanAttr.enChannelType  = HI_UNF_DMX_CHAN_TYPE_POST;
    ChanAttr.enOutputMode   = HI_UNF_DMX_CHAN_OUTPUT_MODE_REC;

    ret = DMX_OsiCreateChannel(DmxId, &ChanAttr, &ChanBuf, ChanId);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    ret = DMX_OsiSetChannelPid(*ChanId, Pid);
    if (HI_SUCCESS != ret)
    {
        DMX_OsiDestroyChannel(*ChanId);

        return ret;
    }

    ret = DMX_OsiOpenChannel(*ChanId);
    if (HI_SUCCESS != ret)
    {
        DMX_OsiDestroyChannel(*ChanId);

        return ret;
    }

    return HI_SUCCESS;
}

static HI_VOID DelAllDmxChannel(HI_U32 DmxId)
{
    HI_U32 i;

    for (i = 0;  i < DMX_CHANNEL_CNT; i++)
    {
        DMX_ChanInfo_S *ChanInfo = &g_pDmxDevOsi->DmxChanInfo[i];

        if (ChanInfo->DmxId == DmxId)
        {
            DMX_OsiCloseChannel(ChanInfo->ChanId);
            DMX_OsiDestroyChannel(ChanInfo->ChanId);
        }
    }
}

HI_S32 DMX_OsiSaveDmxTs_Start(HI_U32 DmxId, HI_U32 u32RecDmxId)
{
    HI_S32              ret;
    DMX_DEV_OSI_S      *DmxDevOsi   = g_pDmxDevOsi;
    DMX_Sub_DevInfo_S  *DmxInfo     = &DmxDevOsi->SubDevInfo[DmxId];
    DMX_PCR_Info_S     *PcrInfo     = DmxDevOsi->DmxPcrInfo;
    DMX_ChanInfo_S     *ChanInfo    = DmxDevOsi->DmxChanInfo;
    HI_U32              ChanId = 0xffffffff;
    HI_U32              i;

    ret = DMX_OsiAttachPort(u32RecDmxId, DmxInfo->PortMode, DmxInfo->PortId);
    if (HI_SUCCESS != ret)
    {
        return ret;
    }

    for (i = 0; i < DMX_CHANNEL_CNT; i++)
    {
        if ((ChanInfo[i].DmxId == DmxId) && (HI_UNF_DMX_CHAN_CLOSE != ChanInfo[i].ChanStatus))
        {
            DMX_OsiAddRecPid(u32RecDmxId, ChanInfo[i].ChanPid, &ChanId);

        #ifdef DMX_DESCRAMBLER_SUPPORT
            if (DMX_INVALID_KEY_ID != ChanInfo[i].KeyId)
            {
            	if ( g_pDscFunc != NULL )
		{
		    g_pDscFunc->DMX_OsiDescramblerAttach(ChanInfo[i].KeyId, ChanId);
		}
		else
		{
		    HI_ERR_DEMUX("error:g_pDscFunc have not beed registered\n");
		}	                
            }
        #endif
        }
    }

    return 0;
}

HI_S32 DMX_OsiSaveDmxTs_Stop(HI_U32 u32RecDmx)
{
    DMX_OsiDetachPort(u32RecDmx);
    DelAllDmxChannel(u32RecDmx);
    return HI_SUCCESS;
}
/*******************************************************************/
#endif

