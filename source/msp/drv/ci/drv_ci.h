#ifndef __DRV_CI_H__
#define __DRV_CI_H__

#include "hal_ci.h"

#ifdef __cplusplus
 #if __cplusplus
extern "C" {
 #endif
#endif

#define DEF_CIMAX_SMI_BITWIDTH (8)          /* CIMAX SMI bit width */
#define DEF_CIMAX_SMC_BANK0_BASE_ADDR (0x30000000)
#define DEF_CIMAX_I2CNum (3)                /* CIMaX I2C group */
#define DEF_CIMAX_ADDR (0x80)               /* CIMaX I2C address */

#define MAX_CIS_SIZE (512)

/* CI driver configure structure */
typedef struct hiCI_PCCD_DEBUGINFO_S
{
    HI_UNF_CI_PCCD_STATUS_E     enStatus;
    HI_UNF_CI_PCCD_READY_E      enReady;
    HI_UNF_CI_PCCD_ACCESSMODE_E enAccessMode;
    HI_BOOL                     bByPass;
} CI_PCCD_DEBUGINFO_S, *CI_PCCD_DEBUGINFO_S_PTR;

HI_S32	CI_Init(HI_VOID);
HI_VOID CI_DeInit(HI_VOID);
HI_S32	CI_Open(HI_UNF_CI_PORT_E enCIPort);
HI_VOID CI_Close(HI_UNF_CI_PORT_E enCIPort);
HI_S32	CI_Standby(HI_UNF_CI_PORT_E enCIPort);
HI_S32	CI_Resume(HI_UNF_CI_PORT_E enCIPort);

HI_S32	CI_PCCD_Open(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);
HI_VOID CI_PCCD_Close(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);

HI_S32	CI_PCCD_CtrlPower(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                          HI_UNF_CI_PCCD_CTRLPOWER_E enCtrlPower);
HI_S32	CI_PCCD_Reset(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);
HI_S32	CI_PCCD_IsReady(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                        HI_UNF_CI_PCCD_READY_E_PTR penCardReady);
HI_S32	CI_PCCD_Detect(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                       HI_UNF_CI_PCCD_STATUS_E_PTR penCardIdStatus);
HI_S32	CI_PCCD_SetAccessMode(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                              HI_UNF_CI_PCCD_ACCESSMODE_E enAccessMode);

HI_S32	CI_PCCD_GetStatus(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId, HI_U8 *pu8Value);

HI_S32	CI_PCCD_IORead(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId, HI_U8 *pu8Buffer, HI_U32 *pu32ReadLen);
HI_S32	CI_PCCD_IOWrite(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                        const HI_U8 *pu8Buffer, HI_U32 u32WriteLen, HI_U32 *pu32WriteOKLen);

HI_S32	CI_PCCD_CheckCIS(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);
HI_S32	CI_PCCD_WriteCOR(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);
HI_S32	CI_PCCD_IOReset(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId);
HI_S32	CI_PCCD_NegBufferSize(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId, HI_U16 *pu16BufferSize);

HI_S32	CI_PCCD_TSCtrl(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                        HI_UNF_CI_PCCD_TSCTRL_E enCMD, HI_VOID *pParam);
HI_S32 CI_PCCD_DbgTSCtrl(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                        HI_UNF_CI_PCCD_TSCTRL_E enCMD, HI_VOID *pParam);
HI_S32 CI_PCCD_DbgIOPrintCtrl(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId, HI_BOOL bPrint);
HI_S32	CI_PCCD_GetDebugInfo(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_PCCD_E enCardId,
                              CI_PCCD_DEBUGINFO_S_PTR pstDebugInfo);

/* Added begin 2012-04-24 l00185424: support various CI device */
HI_S32 CI_SetAttr(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_ATTR_S stCIAttr);
HI_S32 CI_GetAttr(HI_UNF_CI_PORT_E enCIPort, HI_UNF_CI_ATTR_S *pstCIAttr);
/* Added end 2012-04-24 l00185424: support various CI device */

#ifdef __cplusplus
 #if __cplusplus
}
 #endif
#endif

#endif
